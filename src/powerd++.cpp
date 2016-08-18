/** \file
 * Implements powerd++ a drop in replacement for FreeBSD's powerd.
 */

#include "Options.hpp"

#include "fixme.hpp"
#include "types.hpp"
#include "constants.hpp"
#include "errors.hpp"
#include "clas.hpp"
#include "utility.hpp"

#include "sys/sysctl.hpp"
#include "sys/pidfile.hpp"

#include <iostream>  /* std::cout, std::cerr */
#include <locale>    /* std::tolower() */
#include <memory>    /* std::unique_ptr */
#include <chrono>    /* std::chrono::steady_clock::now() */
#include <thread>    /* std::this_thread::sleep_until() */
#include <algorithm> /* std::min(), std::max() */

#include <cstdlib>   /* atof(), atoi(), strtol() */
#include <cstdint>   /* uint64_t */

#include <sys/resource.h>  /* CPUSTATES */

#include <signal.h>  /* signal() */

/**
 * File local scope.
 */
namespace {

using nih::Option;
using nih::make_Options;

using fixme::to_string;

using types::cptime_t;
using types::mhz_t;
using types::coreid_t;
using types::ms;

using errors::Exit;
using errors::Exception;
using errors::fail;

using clas::load;
using clas::freq;
using clas::ival;
using clas::samples;

using utility::countof;
using utility::operator "" _s;
using utility::sprintf;

using constants::CP_TIMES;
using constants::ACLINE;
using constants::FREQ;
using constants::FREQ_LEVELS;

using constants::FREQ_DEFAULT_MAX;
using constants::FREQ_DEFAULT_MIN;
using constants::FREQ_UNSET;
using constants::POWERD_PIDFILE;
using constants::ADP;
using constants::HADP;

/**
 * The available AC line states.
 */
enum class AcLineState : unsigned int {
	BATTERY, /**< Battery is power source */
	ONLINE,  /**< External power source */
	UNKNOWN  /**< Unknown power source */
};

/**
 * Convert an AcLineState into the underlying type.
 *
 * @param op
 *	The operand to convert
 * @return
 *	The integer representation of the operand
 */
constexpr unsigned int to_value(AcLineState const op) {
	return static_cast<unsigned int>(op);
}

/**
 * String descriptions for the AC line states.
 */
const char * const AcLineStateStr[]{"battery", "online", "unknown"};

/**
 * Contains the management information for a single CPU core.
 */
struct Core {
	/**
	 * The sysctl kern.cpu.N.freq, if present.
	 */
	sys::ctl::SysctlSync<mhz_t, 4> freq{{}};

	/**
	 * The core that controls the frequency for this core.
	 */
	coreid_t controller{-1};

	/**
	 * The load during the last frame, a value in the range [0, 1024].
	 */
	cptime_t load{0};

	/**
	 * The minimum core clock rate.
	 */
	mhz_t min{FREQ_DEFAULT_MIN};

	/**
	 * The maximum core clock rate.
	 */
	mhz_t max{FREQ_DEFAULT_MAX};
};

/**
 * A collection of all the gloabl, mutable states.
 *
 * This is mostly for semantic clarity.
 */
struct {
	/**
	 * The last signal received, used for terminating.
	 */
	int signal{0};

	/**
	 * The number of cp_times samples to take.
	 *
	 * At least 2 are required.
	 */
	size_t samples{5};

	/**
	 * The polling interval.
	 */
	ms interval{500};

	/**
	 * The current sample.
	 */
	size_t sample{0};

	/**
	 * The number of CPU cores or threads.
	 */
	sys::ctl::SysctlOnce<coreid_t, 2> const ncpu{1, {CTL_HW, HW_NCPU}};

	/**
	 * Per AC line state settings.
	 */
	struct {
		/**
		 * Lowest frequency to set in MHz.
		 */
		mhz_t freq_min;

		/**
		 * Highest frequency to set in MHz.
		 */
		mhz_t freq_max;

		/**
		 * Target load times [0, 1024].
		 *
		 * The value 0 indicates the corresponding fixed frequency setting
		 * from target_freqs should be used.
		 */
		cptime_t target_load;

		/**
		 * Fixed clock frequencies to use if the target load is set to 0.
		 */
		mhz_t target_freq;
	} acstates[3] {
		{FREQ_UNSET,       FREQ_UNSET,       ADP,  0},
		{FREQ_UNSET,       FREQ_UNSET,       HADP, 0},
		{FREQ_DEFAULT_MIN, FREQ_DEFAULT_MAX, HADP, 0}
	};

	/**
	 * The hw.acpi.acline sysctl.
	 */
	sys::ctl::Sysctl<3> acline_ctl{};

	/**
	 * Verbose mode.
	 */
	bool verbose{false};

	/**
	 * Foreground mode.
	 */
	bool foreground{false};

	/**
	 * Name of an alternative pidfile.
	 *
	 * If not given pidfile_open() uses a default name.
	 */
	char const * pidfilename{POWERD_PIDFILE};

	/**
	 * The kern.cp_times sysctl.
	 */
	sys::ctl::Sysctl<2> cp_times_ctl{};

	/**
	 * This is a ring buffer to be allocated with ncpu × samples
	 * instances of cptime_t[CPUSTATES].
	 */
	std::unique_ptr<cptime_t[][CPUSTATES]> cp_times;

	/**
	 * This buffer is to be allocated with ncpu instances of the
	 * Core struct to store the management information of every
	 * core.
	 */
	std::unique_ptr<Core[]> cores;
} g;

static_assert(countof(g.acstates) == countof(AcLineStateStr),
              "There must be a configuration tuple for each state");

/**
 * Outputs the given message on stderr if g.verbose is set.
 *
 * @param msg
 *	The message to output
 */
inline void verbose(std::string const & msg) {
	if (g.verbose) {
		std::cerr << "powerd++: " << msg << '\n';
	}
}

/**
 * Treat sysctl errors.
 *
 * Fails appropriately for the given error.
 *
 * @param err
 *	The errno value after calling sysctl
 */
[[noreturn]] inline
void sysctl_fail(sys::sc_error<sys::ctl::error> const err) {
	fail(Exit::ESYSCTL, err, "sysctl failed: "_s + err.c_str());
}

/**
 * Perform initial tasks.
 *
 * - Get number of CPU cores/threads
 * - Determine the clock controlling core for each core
 * - Set the MIBs of hw.acpi.acline and kern.cp_times
 */
void init() {
	/* get AC line state MIB */
	try {
		g.acline_ctl = {ACLINE};
	} catch (sys::sc_error<sys::ctl::error>) {
		verbose("cannot read "_s + ACLINE);
	}

	/*
	 * Get the frequency controlling core for each core.
	 * Basically acts as if the kernel supported local frequency changes.
	 */
	g.cores = std::unique_ptr<Core[]>(new Core[g.ncpu]);
	coreid_t controller = -1;
	for (coreid_t core = 0; core < g.ncpu; ++core) {
		char name[40];
		sprintf(name, FREQ, core);
		try {
			g.cores[core].freq = {{name}};
			controller = core;
		} catch (sys::sc_error<sys::ctl::error> e) {
			if (e == ENOENT) {
				verbose("cannot access sysctl: "_s + name);
				if (0 > controller) {
					fail(Exit::ENOFREQ, e, "at least the first CPU core must support frequency updates");
				}
			} else {
				sysctl_fail(e);
			}
		}
		g.cores[core].controller = controller;
	}

	/* set user frequency boundaries */
	auto const line_unknown = to_value(AcLineState::UNKNOWN);
	for (auto & state : g.acstates) {
		if (state.freq_min == FREQ_UNSET) {
			state.freq_min = g.acstates[line_unknown].freq_min;
		}
		if (state.freq_max == FREQ_UNSET) {
			state.freq_max = g.acstates[line_unknown].freq_max;
		}
	}

	/* set per core min/max frequency boundaries */
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		auto & core = g.cores[i];
		if (core.controller != i) { continue; }
		char name[40];
		sprintf(name, FREQ_LEVELS, i);
		try {
			sys::ctl::Sysctl<4> const ctl{name};
			auto levels = ctl.get<char>();
			/* the maximum should at least be the minimum
			 * and vice versa */
			core.max = FREQ_DEFAULT_MIN;
			core.min = FREQ_DEFAULT_MAX;
			for (auto pch = levels.get(); *pch; ++pch) {
				mhz_t freq = strtol(pch, &pch, 10);
				if (pch[0] != '/') { break; }
				core.max = std::max(core.max, freq);
				core.min = std::min(core.min, freq);
				strtol(++pch, &pch, 10);
				/* no idea what that value means */
				if (pch[0] != ' ') { break; }
			}
			assert(core.min <= core.max &&
			       "minimum must not be greater than maximum");
		} catch (sys::sc_error<sys::ctl::error>) {
			verbose("cannot access sysctl: "_s + name);
		}
	}

	/* MIB for kern.cp_times */
	g.cp_times_ctl = {CP_TIMES};
	/* create buffer for system load times */
	g.cp_times = std::unique_ptr<cptime_t[][CPUSTATES]>(
	    new cptime_t[g.samples * g.ncpu][CPUSTATES]{});
}

/**
 * Updates the cp_times ring buffer and the load times for each core.
 */
void update_cp_times() {
	try {
		g.cp_times_ctl.get(g.cp_times[g.sample * g.ncpu],
		                   g.ncpu * sizeof(g.cp_times[0]));
	} catch (sys::sc_error<sys::ctl::error> e) {
		sysctl_fail(e);
	}

	for (coreid_t core = 0; core < g.ncpu; ++core) {
		auto const & cp_times = g.cp_times[g.sample * g.ncpu + core];
		auto const & cp_times_old = g.cp_times[((g.sample + 1) % g.samples) * g.ncpu + core];
		cptime_t all = 0;
		for (size_t i = 0; i < CPUSTATES; ++i) {
			all += cp_times[i] - cp_times_old[i];
		}

		cptime_t idle = cp_times[CP_IDLE] - cp_times_old[CP_IDLE];
		g.cores[core].load = all ? (((all - idle) << 10) / all) : 0;
	}
	g.sample = (g.sample + 1) % g.samples;
}

/**
 * Sets the load time of each clock controlling core to the maximum load
 * in the group.
 */
void update_load_times() {
	update_cp_times();

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto const & core = g.cores[corei];
		if (core.controller == corei) { continue; }
		auto & controller = g.cores[core.controller];
		controller.load = std::max(controller.load, core.load);
	}
}

/**
 * Update the CPU clocks depending on the AC line state and targets.
 */
void update_freq() {
	update_load_times();

	/* get AC line status */
	auto const acline = to_value(sys::ctl::once(AcLineState::UNKNOWN,
	                                            g.acline_ctl));
	auto const & acstate = g.acstates[acline];

	assert(acstate.target_load <= 1024 &&
	       "load target must be in the range [0, 1024]");

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto & core = g.cores[corei];
		if (core.controller != corei) { continue; }

		/* get old clock */
		mhz_t const oldfreq = core.freq;
		/* determine target frequency */
		mhz_t wantfreq = 0;
		if (acstate.target_load) {
			/* adaptive frequency mode */
			assert(oldfreq == ((oldfreq << 10) >> 10) &&
			       "CPU clock frequency exceeds values that are safe to compute");
			wantfreq = oldfreq * core.load / acstate.target_load;
		} else {
			/* fixed frequency mode */
			wantfreq = acstate.target_freq;
		}
		/* apply limits */
		auto const max = std::min(core.max, acstate.freq_max);
		auto const min = std::max(core.min, acstate.freq_min);
		mhz_t newfreq = std::min(std::max(wantfreq, min), max);
		/* update CPU frequency */
		if (oldfreq != newfreq) {
			core.freq = newfreq;
		}
		/* verbose output */
		if (!g.foreground) { continue; }
		std::cout << std::right
		          << "power: " << std::setw(7)
		          << AcLineStateStr[acline]
		          << ", load: " << std::setw(3)
		          << ((core.load * 100 + 512) / 1024)
		          << "%, cpu" << corei << ".freq: "
		          << std::setw(4) << oldfreq << " MHz"
		             ", wanted: " << std::setw(4) << wantfreq << " MHz\n";
	}
	if (g.foreground) { std::cout << std::flush; }
}

/**
 * Fill the cp_times buffer with n - 1 samples.
 */
void reset_cp_times() {
	for (size_t i = 1; i < g.samples; ++i) {
		update_cp_times();
	}
}

/**
 * Sets a load target or fixed frequency for the given AC line state.
 *
 * The string must be in the following format:
 *
 * \verbatim
 * mode_predefined = "minimum" | "min" | "maximum" | "max" |
 *                   "adaptive" | "adp" | "hiadptive" | "hadp";
 * mode =            mode_predefined | load | freq;
 * \endverbatim
 *
 * Scalar values are treated as loads.
 *
 * The predefined values have the following meaning:
 *
 * | Symbol     | Meaning                                      |
 * |------------|----------------------------------------------|
 * | minimum    | The minimum clock rate (default 0 MHz)       |
 * | min        |                                              |
 * | maximum    | The maximum clock rate (default 1000000 MHz) |
 * | max        |                                              |
 * | adaptive   | A target load of 50%                         |
 * | adp        |                                              |
 * | hiadptive  | A target load of 37.5%                       |
 * | hadp       |                                              |
 *
 * @param line
 *	The power line state to set the mode for
 * @param str
 *	A mode string
 */
void set_mode(AcLineState const line, char const * const str) {
	std::string mode{str};
	for (char & ch : mode) { ch = std::tolower(ch); }

	auto const acline = to_value(line);
	auto & acstate = g.acstates[acline];

	acstate.target_load = 0;
	acstate.target_freq = 0;

	if (mode == "minimum" || mode == "min") {
		acstate.target_freq = FREQ_DEFAULT_MIN;
		return;
	}
	if (mode == "maximum" || mode == "max") {
		acstate.target_freq = FREQ_DEFAULT_MAX;
		return;
	}
	if (mode == "adaptive" || mode == "adp") {
		acstate.target_load = ADP;
		return;
	}
	if (mode == "hiadaptive" || mode == "hadp") {
		acstate.target_load = HADP;
		return;
	}

	/* try to set load,
	 * do that first so it gets the scalar values */
	try {
		acstate.target_load = load(str);
		return;
	} catch (Exception & e) {
		if (e.exitcode == Exit::EOUTOFRANGE) { throw; }
	}

	/* try to set clock frequency */
	try {
		acstate.target_freq = freq(str);
		return;
	} catch (Exception & e) {
		if (e.exitcode == Exit::EOUTOFRANGE) { throw; }
	}

	fail(Exit::EMODE, 0, "mode not recognised: "_s + str);
}

/**
 * An enum for command line parsing.
 */
enum class OE {
	USAGE,           /**< Print help */
	MODE_AC,         /**< Set AC power mode */
	MODE_BATT,       /**< Set battery power mode */
	FREQ_MIN,        /**< Set minimum clock frequency */
	FREQ_MAX,        /**< Set maximum clock frequency */
	FREQ_MIN_AC,     /**< Set minimum clock frequency on AC power */
	FREQ_MAX_AC,     /**< Set maximum clock frequency on AC power */
	FREQ_MIN_BATT,   /**< Set minimum clock frequency on battery power */
	FREQ_MAX_BATT,   /**< Set maximum clock frequency on battery power */
	MODE_UNKNOWN,    /**< Set unknown power source mode */
	IVAL_POLL,       /**< Set polling interval */
	FILE_PID,        /**< Set pidfile */
	FLAG_VERBOSE,    /**< Activate verbose output on stderr */
	FLAG_FOREGROUND, /**< Stay in foreground, log events to stdout */
	CNT_SAMPLES,     /**< Set number of cp_times samples */
	IGNORE,          /**< Legacy settings */
	OPT_UNKNOWN,     /**< Obligatory */
	OPT_NOOPT,       /**< Obligatory */
	OPT_DASH,        /**< Obligatory */
	OPT_LDASH,       /**< Obligatory */
	OPT_DONE         /**< Obligatory */
};

/**
 * The short usage string.
 */
char const * const USAGE = "[-hvf] [-abn mode] [-mM freq] [-p ival] [-s cnt] [-P file]";

/**
 * Definitions of command line options.
 */
Option<OE> const OPTIONS[]{
	{OE::USAGE,           'h', "help",       "",     "Show usage and exit"},
	{OE::FLAG_VERBOSE,    'v', "verbose",    "",     "Be verbose"},
	{OE::FLAG_FOREGROUND, 'f', "foreground", "",     "Stay in foreground"},
	{OE::MODE_AC,         'a', "ac",         "mode", "Select the mode while on AC power"},
	{OE::MODE_BATT,       'b', "batt",       "mode", "Select the mode while on battery power"},
	{OE::MODE_UNKNOWN,    'n', "unknown",    "mode", "Select the mode while power source is unknown"},
	{OE::FREQ_MIN,        'm', "min",        "freq", "The minimum CPU frequency"},
	{OE::FREQ_MAX,        'M', "max",        "freq", "The maximum CPU frequency"},
	{OE::FREQ_MIN_AC,      0 , "min-ac",     "freq", "The minimum CPU frequency on AC power"},
	{OE::FREQ_MAX_AC,      0 , "max-ac",     "freq", "The maximum CPU frequency on AC power"},
	{OE::FREQ_MIN_BATT,    0 , "min-batt",   "freq", "The minimum CPU frequency on battery power"},
	{OE::FREQ_MAX_BATT,    0 , "max-batt",   "freq", "The maximum CPU frequency on battery power"},
	{OE::IVAL_POLL,       'p', "poll",       "ival", "The polling interval"},
	{OE::CNT_SAMPLES,     's', "samples",    "cnt",  "The number of samples to use"},
	{OE::FILE_PID,        'P', "pid",        "file", "Alternative PID file"},
	{OE::IGNORE,          'i', "",           "load", "Ignored"},
	{OE::IGNORE,          'r', "",           "load", "Ignored"}
};

/**
 * Parse command line arguments.
 *
 * @param argc,argv
 *	The command line arguments
 */
void read_args(int const argc, char const * const argv[]) {
	auto getopt = make_Options(argc, argv, USAGE, OPTIONS);

	while (true) switch (getopt()) {
	case OE::USAGE:
		throw Exception{Exit::OK, 0, getopt.usage()};
	case OE::FLAG_VERBOSE:
		g.verbose = true;
		break;
	case OE::FLAG_FOREGROUND:
		g.foreground = true;
		break;
	case OE::MODE_AC:
		set_mode(AcLineState::ONLINE, getopt[1]);
		break;
	case OE::MODE_BATT:
		set_mode(AcLineState::BATTERY, getopt[1]);
		break;
	case OE::MODE_UNKNOWN:
		set_mode(AcLineState::UNKNOWN, getopt[1]);
		break;
	case OE::FREQ_MIN:
		g.acstates[to_value(AcLineState::UNKNOWN)]
		    .freq_min = freq(getopt[1]);
		break;
	case OE::FREQ_MAX:
		g.acstates[to_value(AcLineState::UNKNOWN)]
		    .freq_max = freq(getopt[1]);
		break;
	case OE::FREQ_MIN_AC:
		g.acstates[to_value(AcLineState::ONLINE)]
		    .freq_min = freq(getopt[1]);
		break;
	case OE::FREQ_MAX_AC:
		g.acstates[to_value(AcLineState::ONLINE)]
		    .freq_max = freq(getopt[1]);
		break;
	case OE::FREQ_MIN_BATT:
		g.acstates[to_value(AcLineState::BATTERY)]
		    .freq_min = freq(getopt[1]);
		break;
	case OE::FREQ_MAX_BATT:
		g.acstates[to_value(AcLineState::BATTERY)]
		    .freq_max = freq(getopt[1]);
		break;
	case OE::IVAL_POLL:
		g.interval = ival(getopt[1]);
		break;
	case OE::CNT_SAMPLES:
		g.samples = samples(getopt[1]);
		break;
	case OE::FILE_PID:
		g.pidfilename = getopt[1];
		break;
	case OE::IGNORE:
		/* for compatibility with powerd, ignore */
		break;
	case OE::OPT_UNKNOWN:
	case OE::OPT_NOOPT:
	case OE::OPT_DASH:
	case OE::OPT_LDASH:
		fail(Exit::ECLARG, 0, "unexpected command line argument: "_s +
		                      getopt[0] + "\n\n" + getopt.usage());
	case OE::OPT_DONE:
		return;
	}
}

/**
 * Prints the configuration on stderr in verbose mode.
 */
void show_settings() {
	if (!g.verbose) {
		return;
	}
	std::cerr << "Terminal Output\n"
	          << "\tverbose:               yes\n"
	          << "\tforeground:            " << (g.foreground ? "yes\n" : "no\n")
	          << "Load Sampling\n"
	          << "\tcp_time samples:       " << g.samples << '\n'
	          << "\tpolling interval:      " << g.interval.count() << " ms\n"
	          << "\tload average over:     " << ((g.samples - 1) *
	                                             g.interval.count()) << " ms\n"
	          << "Frequency Limits\n";
	for (size_t i = 0; i < countof(g.acstates); ++i) {
		std::cerr << '\t' << std::left << std::setw(23)
		          << (""_s + AcLineStateStr[i] + ':')
		          << '[' << g.acstates[i].freq_min << " MHz, "
		                 << g.acstates[i].freq_max << " MHz]\n";
	}
	std::cerr << "CPU Cores\n"
	          << "\tCPU cores:             " << g.ncpu << '\n'
	          << "Core Groups\n";
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		if (i == g.cores[i].controller) {
			if (i > 0) {
				std::cerr << (i - 1) << "]\n";
			}
			std::cerr << '\t' << i << ": [" << i << ", ";
		}
	}
	std::cerr << (g.ncpu - 1) << "]\n"
	          << "Core Frequency Limits\n";
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		if (i != g.cores[i].controller) { continue; }
		std::cerr << '\t' << i << ": [" << g.cores[i].min << " MHz, "
		          << g.cores[i].max << " MHz]\n";
	}
	std::cerr << "Load Targets\n";
	for (size_t i = 0; i < countof(g.acstates); ++i) {
		auto const & acstate = g.acstates[i];
		std::cerr << '\t' << std::left << std::setw(23)
		          << (""_s + AcLineStateStr[i] + " power target:")
		          << (acstate.target_load ?
		              ((acstate.target_load * 100 + 512) / 1024) :
		              acstate.target_freq)
		          << (acstate.target_load ? "% load\n" : " MHz\n");
	}
}

/**
 * A core frequency guard.
 *
 * This uses the RAII pattern to achieve two things:
 *
 * - Upon creation it reads and writes all controlling cores
 * - Upon destruction it sets all cores to the maximum frequencies
 */
struct FreqGuard final {
	/**
	 * Read and write all core frequencies, may throw.
	 */
	FreqGuard() {
		for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
			auto & core = g.cores[corei];
			if (core.controller != corei) { continue; }
			try {
				core.freq = mhz_t{core.freq};
			} catch (sys::sc_error<sys::ctl::error> e) {
				if (EPERM == e) {
					fail(Exit::EFORBIDDEN, e,
					     "insufficient privileges to change core frequency");
				} else {
					sysctl_fail(e);
				}
			}
		}
	}

	/**
	 * Try to set all core frequencies to the maximum, should not throw.
	 *
	 * This may not alwas be the optimal approach, but it is arguably
	 * sane.
	 */
	~FreqGuard() {
		for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
			auto & core = g.cores[corei];
			if (core.controller != corei) { continue; }
			try {
				core.freq = core.max;
			} catch (sys::sc_error<sys::ctl::error>) {
				/* do nada */
			}
		}
	}
};

/**
 * Sets g.signal, terminating the main loop.
 *
 * @param signal
 *	The signal number received
 */
void signal_recv(int const signal) {
	g.signal = signal;
}

/**
 * Daemonise and run the main loop.
 */
void run_daemon() {
	/* open pidfile */
	sys::pid::Pidfile pidfile{g.pidfilename, 0600};

	/* try to set frequencies once, before detaching from the terminal */
	FreqGuard fguard;

	/* detach from the terminal */
	if (!g.foreground && -1 == ::daemon(0, 1)) {
		fail(Exit::EDAEMON, errno, "detaching the process failed");
	}

	/* Setup SIGHUP */
	if (g.foreground) {
		/* Terminate in foreground */
		signal(SIGHUP, signal_recv);
	} else {
		/* Ignore in daemon mode */
		signal(SIGHUP, SIG_IGN);
	}

	/* write pid */
	try {
		pidfile.write();
	} catch (sys::sc_error<sys::pid::error> e) {
		fail(Exit::EPID, e,
		     "cannot write to pidfile: "_s + g.pidfilename);
	}

	/* the main loop */
	auto time = std::chrono::steady_clock::now();
	while (g.signal == 0) {
		std::this_thread::sleep_until(time += g.interval);
		update_freq();
	}

	verbose("signal "_s + to_string(g.signal) + " received, exiting ...");
}

} /* namespace */

/**
 * Main routine, setup and execute daemon, print errors.
 *
 * @param argc,argv
 *	The command line arguments
 * @return
 *	An exit code
 * @see Exit
 */
int main(int argc, char * argv[]) {
	try {
		signal(SIGINT, signal_recv);
		signal(SIGTERM, signal_recv);
		read_args(argc, argv);
		init();
		show_settings();
		reset_cp_times();
		try {
			run_daemon();
		} catch (pid_t otherpid) {
			fail(Exit::ECONFLICT, EEXIST,
			     "a power daemon is already running under PID: "_s +
			     to_string(otherpid));
		} catch (sys::sc_error<sys::pid::error> e) {
			fail(Exit::EPID, e,
			     "cannot create pidfile "_s + g.pidfilename);
		}
	} catch (Exception & e) {
		if (e.msg != "") {
			std::cerr << e.msg << '\n';
		}
		return to_value(e.exitcode);
	} catch (sys::sc_error<sys::ctl::error> e) {
		std::cerr << "powerd++: untreated sysctl failure: " << e.c_str() << '\n';
		throw;
	} catch (sys::sc_error<sys::pid::error> e) {
		std::cerr << "powerd++: untreated pidfile failure: " << e.c_str() << '\n';
		throw;
	} catch (...) {
		std::cerr << "powerd++: untreated failure\n";
		throw;
	}
	return 0;
}

