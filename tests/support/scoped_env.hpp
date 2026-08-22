#pragma once

// Portable environment-variable helpers for the test suite.
//
// POSIX setenv()/unsetenv() do not exist in the Windows CRT, which builds the
// suite with MinGW. _putenv_s() is the Windows spelling, and assigning an empty
// value there removes the variable outright.

#include <cstdlib>
#include <string>

namespace cyber::test {

inline void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

inline void unsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

// Sets a variable for the lifetime of the object and restores whatever was
// there before (including "absent"), so a failing assertion cannot leak state
// into the tests that run after it.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : m_name(name) {
        if (const char* previous = std::getenv(name)) {
            m_had = true;
            m_previous = previous;
        }
        setEnv(name, value);
    }

    ~ScopedEnv() {
        if (m_had) {
            setEnv(m_name.c_str(), m_previous.c_str());
        } else {
            unsetEnv(m_name.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

private:
    std::string m_name;
    std::string m_previous;
    bool m_had = false;
};

}  // namespace cyber::test
