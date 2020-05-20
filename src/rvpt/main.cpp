//
// Created by AregevDev on 23/04/2020.
//

#include <iostream>

#include "rvpt.h"

int main()
{
    window::settings settings;
    settings.height = 512;
    settings.width = 512;
    window window(settings);
    RVPT rvpt(window);
    bool rvpt_init_ret = rvpt.initialize();
    if (!rvpt_init_ret)
    {
        std::cout << "failed to initialize RVPT\n";
        return 0;
    }

    while (!window.should_close())
    {
        glfwPollEvents();
    }

    return 0;
}