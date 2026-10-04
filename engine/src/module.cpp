// com entry points. the class factory is wrl's; ThumbnailProvider, PropertyHandler and PropertyReader
// register themselves with it through CoCreatableClass.

#include "badge.h"

#include <wrl/module.h>

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    return Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule().GetClassObject(clsid, iid, object);
}

STDAPI DllCanUnloadNow() {
    if (!Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule().Terminate()) {
        return S_FALSE;
    }
    // com unloads the dll next, outside the loader lock, where direct2d may be released
    skyggn::release_drawing_resources();
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
