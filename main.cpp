// В файле с WinMain (например, main.cpp)
#include "Window.h"
#include "DirectXApp.h"
#include <memory>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    auto window = std::make_unique<Window>(hInstance, 1280, 720, L"DirectX 12 Application - Free Camera");
    if (!window->Initialize())
    {
        MessageBox(nullptr, L"Window initialization failed!", L"Error", MB_OK);
        return -1;
    }

    auto app = std::make_unique<DirectXApp>(
        window->GetHWND(),
        window->GetWidth(),
        window->GetHeight(),
        window->GetInputDevice());  // Передаем InputDevice в приложение

    window->OnResize = [&app](int w, int h)
        {
            if (app) app->Resize(w, h);
        };

    if (!app->Initialize())
        return -1;

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