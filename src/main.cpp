#include "app/App.hpp"
#include "app/ProcessSetup.hpp"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifdef NDEBUG
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

int main(int argc, char* argv[]) {
    if (!AppShell::ProcessSetup::HandleCommandLine(argc, argv)) {
        return 0;
    }
    AppShell::ProcessSetup processSetup;
    App app;
    return app.Run();
}
