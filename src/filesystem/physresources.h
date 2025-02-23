// ====================================================================================================================
// Created by Retro & Chill on 11/19/2023.
// ------------------------------------------------------------------------------------------------------------------
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
// documentation files (the “Software”), to deal in the Software without restriction, including without limitation the 
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to 
// permit persons to whom the Software is furnished to do so.
// ====================================================================================================================

#pragma once

#include "raiiwrapper.h"
#include "debugwriter.h"

#include <physfs.h>

class PhysResources : public RaiiWrapper {
public:
    explicit PhysResources(const char *argv0)  {
        if (PHYSFS_init(argv0) != 0)
            startupSucceeded();
        else
            startupFailed("Error initializing PhysFS");
    }

    ~PhysResources()  {
        if (startedSuccessfully()) {
            if (PHYSFS_deinit() == 0)
                Debug() << "PhyFS failed to deinit.";
        }
    }

    PhysResources(const PhysResources &) = delete;
    PhysResources(PhysResources&&) = delete;

    PhysResources &operator=(const PhysResources &) = delete;
    PhysResources &operator=(PhysResources &&) = delete;
};
