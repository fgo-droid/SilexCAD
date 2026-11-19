// Silex stub for FreeCAD's Base/Console.h and Base/TimeElapsed (logging is silenced).
#pragma once
#include <chrono>

namespace Base
{
struct ConsoleStub
{
    template<typename... Args>
    void Log(const char*, Args&&...) {}
    template<typename... Args>
    void Warning(const char*, Args&&...) {}
    template<typename... Args>
    void Error(const char*, Args&&...) {}
    template<typename... Args>
    void Message(const char*, Args&&...) {}
};
inline ConsoleStub& Console()
{
    static ConsoleStub c;
    return c;
}
class TimeElapsed
{
public:
    TimeElapsed() : t(std::chrono::steady_clock::now()) {}
    static double diffTimeF(const TimeElapsed& a, const TimeElapsed& b)
    {
        return std::chrono::duration<double>(b.t - a.t).count();
    }
    std::chrono::steady_clock::time_point t;
};
}  // namespace Base
