#pragma once

struct X11Platform {
    Display* display = nullptr;
    int screen = 0;
    Window window = 0;
    Atom wmDeleteWindow = 0;
    bool closeRequested = false;
    int width = 1600;
    int height = 960;
    std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();

    bool create(int initialWidth, int initialHeight, const char* title, std::string* error)
    {
        display = XOpenDisplay(nullptr);
        if (!display) {
            if (error) *error = "Failed to open X11 display.";
            return false;
        }
        screen = DefaultScreen(display);
        width = initialWidth;
        height = initialHeight;

        XSetWindowAttributes attrs{};
        attrs.event_mask = ExposureMask |
            StructureNotifyMask |
            KeyPressMask |
            KeyReleaseMask |
            ButtonPressMask |
            ButtonReleaseMask |
            PointerMotionMask |
            FocusChangeMask;
        window = XCreateWindow(display,
                               RootWindow(display, screen),
                               0,
                               0,
                               static_cast<unsigned int>(width),
                               static_cast<unsigned int>(height),
                               0,
                               CopyFromParent,
                               InputOutput,
                               CopyFromParent,
                               CWEventMask,
                               &attrs);
        if (!window) {
            if (error) *error = "Failed to create X11 window.";
            shutdown();
            return false;
        }
        XStoreName(display, window, title);
        wmDeleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display, window, &wmDeleteWindow, 1);
        XMapWindow(display, window);
        XFlush(display);
        return true;
    }

    void updateModifiers(unsigned int state, KeySym keySym, bool down)
    {
        ImGuiIO& io = ImGui::GetIO();
        bool ctrl = (state & ControlMask) != 0;
        bool shift = (state & ShiftMask) != 0;
        bool alt = (state & Mod1Mask) != 0;
        bool super = (state & Mod4Mask) != 0;
        if (keySym == XK_Control_L || keySym == XK_Control_R) ctrl = down;
        if (keySym == XK_Shift_L || keySym == XK_Shift_R) shift = down;
        if (keySym == XK_Alt_L || keySym == XK_Alt_R) alt = down;
        if (keySym == XK_Super_L || keySym == XK_Super_R) super = down;
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddKeyEvent(ImGuiMod_Alt, alt);
        io.AddKeyEvent(ImGuiMod_Super, super);
    }

    void pollEvents()
    {
        ImGuiIO& io = ImGui::GetIO();
        while (display && XPending(display) > 0) {
            XEvent event{};
            XNextEvent(display, &event);
            switch (event.type) {
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == wmDeleteWindow) {
                    closeRequested = true;
                }
                break;
            case ConfigureNotify:
                width = std::max(1, event.xconfigure.width);
                height = std::max(1, event.xconfigure.height);
                break;
            case MotionNotify:
                io.AddMousePosEvent(static_cast<float>(event.xmotion.x), static_cast<float>(event.xmotion.y));
                break;
            case ButtonPress:
            case ButtonRelease: {
                const bool down = event.type == ButtonPress;
                const unsigned int button = event.xbutton.button;
                if (button == Button1) io.AddMouseButtonEvent(0, down);
                if (button == Button2) io.AddMouseButtonEvent(2, down);
                if (button == Button3) io.AddMouseButtonEvent(1, down);
                if (down && button == Button4) io.AddMouseWheelEvent(0.0f, 1.0f);
                if (down && button == Button5) io.AddMouseWheelEvent(0.0f, -1.0f);
                break;
            }
            case FocusIn:
                io.AddFocusEvent(true);
                break;
            case FocusOut:
                io.AddFocusEvent(false);
                break;
            case KeyPress:
            case KeyRelease: {
                const bool down = event.type == KeyPress;
                KeySym keySym = NoSymbol;
                char text[32]{};
                const int textLength = XLookupString(&event.xkey, text, sizeof(text), &keySym, nullptr);
                updateModifiers(event.xkey.state, keySym, down);
                const ImGuiKey imguiKey = imguiKeyFromX11KeySym(keySym);
                if (imguiKey != ImGuiKey_None) {
                    io.AddKeyEvent(imguiKey, down);
                }
                if (down && textLength > 0) {
                    io.AddInputCharactersUTF8(text);
                }
                break;
            }
            default:
                break;
            }
        }
    }

    void newFrame()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendPlatformName = "jcut_x11";
        io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        const auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::max(1.0f / 1000.0f,
                                static_cast<float>(std::chrono::duration<double>(now - lastFrameTime).count()));
        lastFrameTime = now;
    }

    bool shouldClose() const { return closeRequested; }

    void shutdown()
    {
        if (display && window) {
            XDestroyWindow(display, window);
            window = 0;
        }
        if (display) {
            XCloseDisplay(display);
            display = nullptr;
        }
    }
};
