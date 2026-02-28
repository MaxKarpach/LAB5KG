#include "Window.h"
#include "DirectXApp.h"
#include <memory>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    auto window = std::make_unique<Window>(hInstance, 1280, 720, L"DirectX 12 Application");
    if (!window->Initialize())
    {
        MessageBox(nullptr, L"Window initialization failed!", L"Error", MB_OK);
        return -1;
    }

    auto app = std::make_unique<DirectXApp>(
        window->GetHWND(), window->GetWidth(), window->GetHeight());

    // Явно захватываем app по ссылке
    window->OnResize = [&app](int w, int h)
        {
            if (app) app->Resize(w, h);
        };

    if (!app->Initialize())
        return -1;

    // Явно захватываем window и app по ссылке
    return window->Run(
        [&window, &app](float dt)
        {
            window->GetInputDevice()->Update();
            if (app) app->Update(dt);
        },
        [&app]()
        {
            if (app) app->Render();
        }
    );
}