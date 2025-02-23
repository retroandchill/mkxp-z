// ====================================================================================================================
// Created by Retro & Chill on 11/18/2023.
// ------------------------------------------------------------------------------------------------------------------
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
// documentation files (the “Software”), to deal in the Software without restriction, including without limitation the 
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to 
// permit persons to whom the Software is furnished to do so.
// ====================================================================================================================
#pragma once

#include "raiiwrapper.h"

#include <SDL_image.h>

namespace SDL2 {

    class Image : public RaiiWrapper {
    public:
        explicit Image(int flags) {
            if (IMG_Init(flags) == flags)
                startupSucceeded();
            else
                startupFailed(std::string("Error initializing SDL_image: ") +
                              SDL_GetError());
        }
        ~Image() {
            if (startedSuccessfully())
                IMG_Quit();
        }

        Image(const Image &) = delete;
        Image(Image&&) = delete;

        Image &operator=(const Image &) = delete;
        Image &operator=(Image &&) = delete;
    };

} // SDL2
