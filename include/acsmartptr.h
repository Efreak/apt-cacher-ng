/*
 * acsmartptr.h
 *
 *  Created on: 01.12.2017
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_ACSMARTPTR_H_
#define INCLUDE_ACSMARTPTR_H_

#include <meta.h>

namespace acng {

/**
 * Basic base implementation of a reference-counted class
 */
struct tLintRefcounted
{
private:
	size_t m_nRefCount = 0;
public:
    inline void __inc_ref() noexcept { m_nRefCount++; }
    inline void __dec_ref()
    {
        if(--m_nRefCount == 0)
            delete this;
    }
    virtual ~tLintRefcounted() {}
    inline size_t __ref_cnt() { return m_nRefCount; }
};

/**
 * Two-step destruction with two kinds of references; owning ones (strong count) and remaining ones.
 * When owners are gone, object is considered abandoned and a specific function is run.
 */
struct tRefcountedDetachable : tLintRefcounted
{
private:
    size_t m_nStrongCount = 0;
public:
    inline void __inc_strong_ref() noexcept { m_nStrongCount++; }
    inline void __dec_strong_ref()
    {
        if(--m_nStrongCount == 0)
            abandon();
    }
    virtual void abandon() =0;
    inline size_t __strong_ref_cnt() { return m_nStrongCount; }
};

/**
 * Lightweight intrusive smart pointer with ordinary reference counting
 */
template<class T>
class lint_ptr
{
	T * m_ptr = nullptr;
public:
	explicit lint_ptr()
	{
	}
	explicit lint_ptr(T *rawPtr) :
			m_ptr(rawPtr)
	{
		if(rawPtr)
			rawPtr->__inc_ref();
	}
	lint_ptr(const ::acng::lint_ptr<T> & orig) :
			m_ptr(orig.m_ptr)
	{
		if(!m_ptr) return;
		m_ptr->__inc_ref();
	}
	inline ~lint_ptr<T>()
	{
		if(!m_ptr) return;
		m_ptr->__dec_ref();
	}
	T* get() const
	{
		return m_ptr;
	}
	bool operator==(const T* raw) const
	{
		return get() == raw;
	}
	inline void reset(T *rawPtr) noexcept
	{
		if(rawPtr == m_ptr) // heh?
			return;
		reset();
		m_ptr = rawPtr;
		if(rawPtr)
			rawPtr->__inc_ref();
	}
	inline void swap(lint_ptr<T>& other)
	{
		std::swap(m_ptr, other.m_ptr);
	}
	inline void reset() noexcept
	{
		if(m_ptr)
			m_ptr->__dec_ref();
		m_ptr = nullptr;
	}
	lint_ptr<T>& operator=(const lint_ptr<T> &other)
	{
		if(m_ptr == other.m_ptr)
			return *this;
		reset(other.m_ptr);
		return *this;
	}
	lint_ptr<T>& operator=(lint_ptr<T> &&other)
	{
		if(m_ptr == other.m_ptr)
			return *this;

		m_ptr = other.m_ptr;
		other.m_ptr = nullptr;
		return *this;
	}
	// pointer-like access options
	explicit inline operator bool() const noexcept
	{
		return m_ptr;
	}
	inline T& operator*() const noexcept
	{
		return *m_ptr;
	}
	inline T* operator->() const noexcept
	{
		return m_ptr;
	}
	// pointer-like access options
	inline bool operator<(const lint_ptr<T> &vs) const noexcept
	{
		return m_ptr < vs.m_ptr;
	}
};

/**
 * Alternative version which is supposed to be used by "priviledged" users, which are counted additionaly by "strong count".
 * When all strong pointers are gone, dispose method shall be called.
 */
template<class T>
class lint_strong_ptr
{
	T * m_ptr = nullptr;
public:
	explicit lint_strong_ptr()
	{
	}
	explicit lint_strong_ptr(T *rawPtr) :
			m_ptr(rawPtr)
	{
		if(!m_ptr) return;
		m_ptr->__inc_ref();
		m_ptr->__inc_strong_ref();
	}
	lint_strong_ptr(const ::acng::lint_strong_ptr<T> & orig) :
			m_ptr(orig.m_ptr)
	{
		if(!m_ptr)
			return;
		m_ptr->__inc_ref();
		m_ptr->__inc_strong_ref();
	}
	inline ~lint_strong_ptr<T>()
	{
		if(!m_ptr) return;
		m_ptr->__dec_strong_ref();
		m_ptr->__dec_ref();
	}
	T* get()
	{
		return m_ptr;
	}
	inline void reset(T *rawPtr) noexcept
	{
		if(rawPtr == m_ptr) // heh?
			return;
		reset();
		if(!rawPtr)
			return;
		m_ptr = rawPtr;
		m_ptr->__inc_ref();
		m_ptr->__inc_strong_ref();
	}
	inline void reset() noexcept
	{
		if(m_ptr)
		{
			m_ptr->__dec_strong_ref();
			m_ptr->__dec_ref();
		}
		m_ptr = nullptr;
	}
	lint_strong_ptr<T>& operator=(const lint_strong_ptr<T> &other)
	{
		if(m_ptr == other.m_ptr)
			return *this;
		reset(other.m_ptr);
		return *this;
	}
	lint_strong_ptr<T>& operator=(lint_strong_ptr<T> &&other)
	{
		if(m_ptr == other.m_ptr)
			return *this;
		m_ptr = other.m_ptr;
		other.m_ptr = nullptr;
		return *this;
	}
	// pointer-like access options
	explicit inline operator bool() const noexcept
	{
		return m_ptr;
	}
	inline T& operator*() const noexcept
	{
		return *m_ptr;
	}
	inline T* operator->() const noexcept
	{
		return m_ptr;
	}
	// pointer-like access options
	inline bool operator<(const lint_strong_ptr<T> &vs) const noexcept
	{
		return m_ptr < vs.m_ptr;
	}
};

template<typename C>
inline lint_ptr<C> as_lptr(C* a)
{
	return lint_ptr<C>(a);
};


template<typename C>
inline lint_strong_ptr<C> as_lptr_strong(C* a)
{
	return lint_strong_ptr<C>(a);
};



template<class C>
lint_ptr<C> make_lptr()
{
	return lint_ptr<C>(new C());
};


template<class C, typename Ta>
lint_ptr<C> make_lptr(Ta& a)
{
	return lint_ptr<C>(new C(a));
};

// XXX: convert this to variadic template
template<class C, typename Ta, typename Tb>
lint_ptr<C> make_lptr(Ta& a, Tb& b)
{
	return lint_ptr<C>(new C(a, b));
}

template<class C, typename Ta, typename Tb, typename Tc>
lint_ptr<C> make_lptr(Ta& a, Tb& b, Tc& c)
{
	return lint_ptr<C>(new C(a, b, c));
}

template<class C, typename Ta, typename Tb, typename Tc, typename Td>
lint_ptr<C> make_lptr(Ta& a, Tb& b, Tc& c, Td& d)
{
	return lint_ptr<C>(new C(a, b, c, d));
}

template<class C, typename Ta, typename Tb, typename Tc, typename Td, typename Te>
lint_ptr<C> make_lptr(Ta& a, Tb& b, Tc& c, Td& d, Te &e)
{
	return lint_ptr<C>(new C(a, b, c, d, e));
}

#if 0 // meh, actually architecturally insane
/**
 * Special extension to intrusive pointer, introducing a special counter for "the inner party".
 * This partly mimics the behaviour of IDisposable pattern from .NET by adding two-step destruction.
 */
template<class U>
class spec_use_counted_ptr
{
	lint_ptr<U> p;
public:
	spec_use_counted_ptr() = default;
	spec_use_counted_ptr(U* ptr) : p(ptr) { p->m_nUseCount++; }
	//user_ptr(const user_ptr<U> &p) : m_ptr(p) { p->m_nUseCount++; }
	~spec_use_counted_ptr() { reset(); }
	void reset()
	{
		if(!p) return;
		if(-- p->m_nUseCount == 0)
			p->Dispose();
		p.reset();
	}

	// pointer-like access options
	explicit inline operator bool() const noexcept
	{
		return p;
	}
	inline U& operator*() const noexcept
	{
		return p;
	}
	inline U* operator->() const noexcept
	{
		return p.m_ptr;
	}
};
// sample base class, expected by user_ptr
struct tUseCounted
{
    size_t m_nUseCount = 0;
};
#endif

} // namespace acng

#endif /* INCLUDE_ACSMARTPTR_H_ */
