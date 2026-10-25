#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace AppShell {

    class ProcessSetup {
      public:
        static bool HandleCommandLine(int argc, char* argv[]);

        ProcessSetup();
        ~ProcessSetup();

        ProcessSetup(const ProcessSetup&) = delete;
        ProcessSetup& operator=(const ProcessSetup&) = delete;

      private:
        bool m_initialized = false;
    };

} // namespace AppShell
