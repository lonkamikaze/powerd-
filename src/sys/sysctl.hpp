/** \file
 * Implements safer c++ wrappers for the sysctl() interface.
 */

#ifndef _POWERDXX_SYS_SYSCTL_HPP_
#define _POWERDXX_SYS_SYSCTL_HPP_

#include "error.hpp"       /* sys::sc_error */

#include <memory>          /* std::unique_ptr */

#include <cassert>         /* assert() */

#include <sys/types.h>     /* sysctl() */
#include <sys/sysctl.h>    /* sysctl() */

namespace sys {

/**
 * This namespace contains safer c++ wrappers for the sysctl() interface.
 *
 * The template class Sysctl represents a sysctl address and offers
 * handles to retrieve or set the stored value.
 *
 * The template class Sync represents a sysctl value that is read and
 * written synchronously.
 *
 * The template class Once represents a read once value.
 */
namespace ctl {

/**
 * The domain error type.
 */
struct error {};

/**
 * Management Information Base identifier type (see sysctl(3)).
 */
typedef int mib_t;

/**
 * A wrapper around the sysctl() function.
 *
 * All it does is throw an exception if sysctl() fails.
 *
 * @param name,namelen
 *	The MIB buffer and its length
 * @param oldp,oldlenp
 *	Pointers to the return buffer and its length
 * @param newp,newlen
 *	A pointer to the buffer with the new value and the buffer length
 * @throws sys::sc_error<error>
 *	Throws if sysctl() fails for any reason
 */
inline void sysctl_raw(mib_t const * name, u_int const namelen,
                       void * const oldp, size_t * const oldlenp,
                       void const * const newp, size_t const newlen) {
	if (sysctl(name, namelen, oldp, oldlenp, newp, newlen) == -1) {
		throw sc_error<error>{errno};
	}
}

/**
 * Returns a sysctl() value to a buffer.
 *
 * @tparam MibDepth
 *	The length of the MIB buffer
 * @param mib
 *	The MIB buffer
 * @param oldp,oldlen
 *	A pointers to the return buffer and a reference to its length
 * @throws sys::sc_error<error>
 *	Throws if sysctl() fails for any reason
 */
template <size_t MibDepth>
void sysctl_get(mib_t const (& mib)[MibDepth], void * const oldp, size_t & oldlen) {
	sysctl_raw(mib, MibDepth, oldp, &oldlen, nullptr, 0);
}

/**
 * Sets a sysctl() value.
 *
 * @tparam MibDepth
 *	The length of the MIB buffer
 * @param mib
 *	The MIB buffer
 * @param newp,newlen
 *	A pointer to the buffer with the new value and the buffer length
 * @throws sys::sc_error<error>
 *	Throws if sysctl() fails for any reason
 */
template <size_t MibDepth>
void sysctl_set(mib_t const (& mib)[MibDepth], void const * const newp,
                size_t const newlen) {
	sysctl_raw(mib, MibDepth, nullptr, nullptr, newp, newlen);
}

/**
 * Represents a sysctl MIB address.
 *
 * It offers set() and get() methods to access these sysctls.
 *
 * There are two ways of initialising a Sysctl instance, by symbolic
 * name or by directly using the MIB address. The latter one only
 * makes sense for sysctls with a fixed address, known at compile
 * time, e.g. `Sysctl<2>{CTL_HW, HW_NCPU}` for "hw.ncpu". Check
 * `/usr/include/sys/sysctl.h` for predefined MIBs.
 *
 * For all other sysctls, symbolic names must be used. E.g.
 * `Sysctl<>{"dev.cpu.0.freq"}`. Creating a Sysctl from a symbolic
 * name may throw.
 *
 * Fixed address sysctls may be created using the make_Sysctl() function,
 * e.g. `make_Sysctl(CTL_HW, HW_NCPU)`.
 *
 * Instances created from symbolic names must use the Sysctl<0>
 * specialisation, this can be done by omitting the template argument
 * `Sysctl<>`.
 *
 * @tparam MibDepth
 *	The MIB level, e.g. "hw.ncpu" is two levels deep
 */
template <size_t MibDepth = 0>
class Sysctl {
	private:
	/**
	 * Stores the MIB address.
	 */
	mib_t mib[MibDepth];

	public:
	/**
	 * Initialise the MIB address directly.
	 *
	 * Some important sysctl values have a fixed address that
	 * can be initialised at compile time with a noexcept guarantee.
	 *
	 * Spliting the MIB address into head and tail makes sure
	 * that `Sysctl(char *)` does not match the template and is
	 * instead implicitly cast to invoke `Sysctl(char const *)`.
	 *
	 * @tparam Tail
	 *	The types of the trailing MIB address values (must
	 *	be mib_t)
	 * @param head,tail
	 *	The mib
	 */
	template <typename... Tail>
	constexpr Sysctl(mib_t const head, Tail const... tail) noexcept :
	    mib{head, tail...} {
		static_assert(MibDepth == sizeof...(Tail) + 1,
		              "MIB depth mismatch");
	}

	/**
	 * The size of the sysctl.
	 *
	 * @return
	 *	The size in characters
	 */
	size_t size() const
	{
		size_t len = 0;
		sysctl_get(this->mib, nullptr, len);
		return len;
	}

	/**
	 * Update the given buffer with a value retrieved from the
	 * sysctl.
	 *
	 * @param buf,bufsize
	 *	The target buffer and its size
	 * @throws sys::sc_error<error>
	 *	Throws if value retrieval fails or is incomplete,
	 *	e.g. because the value does not fit into the target
	 *	buffer
	 */
	void get(void * const buf, size_t const bufsize) const {
		auto len = bufsize;
		sysctl_get(this->mib, buf, len);
	}

	/**
	 * Update the given value with a value retreived from the
	 * sysctl.
	 *
	 * @tparam T
	 *	The type store the sysctl value in
	 * @param value
	 *	A reference to the target value
	 * @throws sys::sc_error<error>
	 *	Throws if value retrieval fails or is incomplete,
	 *	e.g. because the value does not fit into the target
	 *	type
	 */
	template <typename T>
	void get(T & value) const {
		get(&value, sizeof(T));
	}

	/**
	 * Retrieve an array from the sysctl address.
	 *
	 * This is useful to retrieve variable length sysctls, like
	 * characer strings.
	 *
	 * @tparam T
	 *	The type stored in the array
	 * @return
	 *	And array of T with the right length to store the
	 *	whole sysctl value
	 * @throws sys::sc_error<error>
	 *	May throw if the size of the sysctl increases after
	 *	the length was queried
	 */
	template <typename T>
	std::unique_ptr<T[]> get() const {
		auto const len = size();
		auto result = std::unique_ptr<T[]>(new T[len / sizeof(T)]);
		get(result.get(), len);
		return result;
	}

	/**
	 * Update the the sysctl value with the given buffer.
	 *
	 * @param buf,bufsize
	 *	The source buffer
	 * @throws sys::sc_error<error>
	 *	If the source buffer cannot be stored in the sysctl
	 */
	void set(void const * const buf, size_t const bufsize) {
		sysctl_set(this->mib, buf, bufsize);
	}

	/**
	 * Update the the sysctl value with the given value.
	 *
	 * @tparam T
	 *	The value type
	 * @param value
	 *	The value to set the sysctl to
	 */
	template <typename T>
	void set(T const & value) {
		set(&value, sizeof(T));
	}
};

/**
 * This is a specialisation of Sysctl for sysctls using symbolic names.
 *
 * A Sysctl instance created with the default constructor is unitialised,
 * initialisation can be deferred to a later moment by using copy assignment.
 * This can be used to create globals but construct them inline where
 * exceptions can be handled.
 */
template <>
class Sysctl<0> {
	private:
	/**
	 * Stores the MIB address.
	 */
	mib_t mib[CTL_MAXNAME];

	/**
	 * The MIB depth.
	 */
	size_t depth;

	public:
	/**
	 * The default constructor.
	 *
	 * This is available to defer initialisation to a later moment.
	 */
	constexpr Sysctl() : mib{}, depth{0} {}

	/**
	 * Initialise the MIB address from a character string.
	 *
	 * @param name
	 *	The symbolic name of the sysctl
	 * @throws sys::sc_error<error>
	 *	May throw an exception if the addressed sysct does
	 *	not exist or if the address is too long to store
	 */
	Sysctl(char const * const name) : depth{CTL_MAXNAME} {
		if (sysctlnametomib(name, this->mib, &this->depth) == -1) {
			throw sc_error<error>{errno};
		}
		assert(this->depth <= CTL_MAXNAME && "MIB depth exceeds limit");
	}

	/**
	 * @copydoc Sysctl::size() const
	 */
	size_t size() const
	{
		size_t len = 0;
		sysctl_raw(this->mib, this->depth, nullptr, &len, nullptr, 0);
		return len;
	}

	/**
	 * @copydoc Sysctl::get(void * const, size_t const) const
	 */
	void get(void * const buf, size_t const bufsize) const {
		auto len = bufsize;
		sysctl_raw(this->mib, this->depth, buf, &len, nullptr, 0);
	}

	/**
	 * @copydoc Sysctl::get(T &) const
	 */
	template <typename T>
	void get(T & value) const {
		get(&value, sizeof(T));
	}

	/**
	 * @copydoc Sysctl::get() const
	 */
	template <typename T>
	std::unique_ptr<T[]> get() const {
		size_t const len = size();
		auto result = std::unique_ptr<T[]>(new T[len / sizeof(T)]);
		get(result.get(), len);
		return result;
	}

	/**
	 * @copydoc Sysctl::set(void const * const, size_t const)
	 */
	void set(void const * const buf, size_t const bufsize) {
		sysctl_raw(this->mib, this->depth, nullptr, nullptr, buf, bufsize);
	}

	/**
	 * @copydoc Sysctl::set(T const &)
	 */
	template <typename T>
	void set(T const & value) {
		set(&value, sizeof(T));
	}
};

/**
 * Create a Sysctl instances.
 *
 * This is only compatible with creating sysctls from predefined MIBs.
 *
 * @tparam Args
 *	List of argument types, should all be pid_t
 * @param args
 *	List of initialising arguments
 * @return
 *	A Sysctl instance with the depth matching the number of arguments
 */
template <typename... Args>
constexpr Sysctl<sizeof...(Args)> make_Sysctl(Args const... args) {
	return {args...};
}

/**
 * This is a wrapper around Sysctl that allows semantically transparent
 * use of a sysctl.
 *
 * ~~~ c++
 * Sync<int, Sysctl<>> sndUnit{{"hw.snd.default_unit"}};
 * if (sndUnit != 3) {    // read from sysctl
 *	sndUnit = 3;      // assign to sysctl
 * }
 * ~~~
 *
 * Note that both assignment and read access (implemented through
 * type casting to T) may throw an exception.
 *
 * @tparam T
 *	The type to represent the sysctl as
 * @tparam SysctlT
 *	The Sysctl type
 */
template <typename T, class SysctlT>
class Sync {
	private:
	/**
	 * A sysctl to represent.
	 */
	SysctlT sysctl;

	public:
	/**
	 * The default constructor.
	 *
	 * This is available to defer initialisation to a later moment.
	 * This might be useful when initialising global or static
	 * instances by a character string repesented name.
	 */
	constexpr Sync() {}

	/**
	 * The constructor copies the given Sysctl instance.
	 *
	 * @param sysctl
	 *	The Sysctl instance to represent
	 */
	constexpr Sync(SysctlT const & sysctl) noexcept : sysctl{sysctl} {}

	/**
	 * Transparently assiges values of type T to the represented
	 * Sysctl instance.
	 *
	 * @param value
	 *	The value to assign
	 * @return
	 *	A self reference
	 */
	Sync & operator =(T const & value) {
		this->sysctl.set(value);
		return *this;
	}

	/**
	 * Implicitly cast to the represented type.
	 *
	 * @return
	 *	Returns the value from the sysctl
	 */
	operator T () const {
		T value;
		this->sysctl.get(value);
		return value;
	}
};

/**
 * A convenience alias around Sync.
 *
 * ~~~ c++
 * // Sync<int, Sysctl<>> sndUnit{{"hw.snd.default_unit"}};
 * SysctlSync<int> sndUnit{{"hw.snd.default_unit"}};
 * if (sndUnit != 3) {    // read from sysctl
 *	sndUnit = 3;      // assign to sysctl
 * }
 * ~~~
 *
 * @tparam T
 *	The type to represent the sysctl as
 * @tparam MibDepth
 *	The MIB depth, provide only for compile time initialisation
 */
template <typename T, size_t MibDepth = 0>
using SysctlSync = Sync<T, Sysctl<MibDepth>>;

/**
 * A read once representation of a Sysctl.
 *
 * This reads a sysctl once upon construction and always returns that
 * value. It does not support assignment.
 *
 * This class is intended for sysctls that are not expected to change,
 * such as hw.ncpu. A special property of this class is that the
 * constructor does not throw and takes a default value in case reading
 * the sysctl fails.
 *
 * ~~~ c++
 * // Read number of CPU cores, assume 1 on failure:
 * Once<coreid_t, Sysctl<2>> ncpu{1, {CTL_HW, HW_NCPU}};
 * // Equivalent:
 * int hw_ncpu;
 * try {
 * 	Sysctl<2>{CTL_HW, HW_NCPU}.get(hw_ncpu);
 * } catch (sys::sc_error<error>) {
 * 	hw_ncpu = 1;
 * }
 * ~~~
 *
 * @tparam T
 *	The type to represent the sysctl as
 * @tparam SysctlT
 *	The Sysctl type
 */
template <typename T, class SysctlT>
class Once {
	private:
	/**
	 * The sysctl value read upon construction.
	 */
	T value;

	public:
	/**
	 * The constructor tries to read and store the requested sysctl.
	 *
	 * If reading the requested sysctl fails for any reason,
	 * the given value is stored instead.
	 *
	 * @param value
	 *	The fallback value
	 * @param sysctl
	 *	The sysctl to represent
	 */
	Once(T const & value, SysctlT const & sysctl) noexcept {
		try {
			sysctl.get(this->value);
		} catch (sc_error<error>) {
			this->value = value;
		}
	}

	/**
	 * Return a const reference to the value.
	 *
	 * @return
	 *	A const reference to the value
	 */
	operator T const &() const {
		return this->value;
	}
};

/**
 * A convenience alias around Once.
 *
 * ~~~ c++
 * // Once<coreid_t, Sysctl<2>> ncpu{0, {CTL_HW, HW_NCPU}};
 * SysctlOnce<coreid_t, 2> ncpu{1, {CTL_HW, HW_NCPU}};
 * ~~~
 *
 * @tparam T
 *	The type to represent the sysctl as
 * @tparam MibDepth
 *	The maximum allowed MIB depth
 */
template <typename T, size_t MibDepth>
using SysctlOnce = Once<T, Sysctl<MibDepth>>;

/**
 * This creates a Once instance.
 *
 * This is intended for cases when a Once instance is created as a
 * temporary to retrieve a value, using it's fallback to a default
 * mechanism.
 *
 * @tparam T
 *	The value type
 * @tparam SysctlT
 *	The Sysctl type
 * @param value
 *	The default value to fall back to
 * @param sysctl
 *	The sysctl to try and read from
 */
template <typename T, class SysctlT>
constexpr Once<T, SysctlT> make_Once(T const & value, SysctlT const & sysctl)
noexcept { return {value, sysctl}; }

} /* namespace ctl */
} /* namespace sys */

#endif /* _POWERDXX_SYS_SYSCTL_HPP_ */
