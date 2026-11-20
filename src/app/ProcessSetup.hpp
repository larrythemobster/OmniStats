#pragma once

namespace AppShell {

    class ProcessSetup {
      public:
        static void InitializeEarly();
        static bool HandleCommandLine(int argc, char* argv[]);
        static bool EnsureSingleInstance();
        static void ReleaseSingleInstance();

        ProcessSetup();
        ~ProcessSetup();

        ProcessSetup(const ProcessSetup&) = delete;
        ProcessSetup& operator=(const ProcessSetup&) = delete;

      private:
        bool m_initialized = false;
    };

} // namespace AppShell
