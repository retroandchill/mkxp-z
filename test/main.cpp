//
// Created by fcors on 10/19/2023.
//

#include "binding-mri.h"
#include <ruby.h>

#include <windows.h>
#include <iostream>
#include <SDL_events.h>

int main(int argc, char **argv) {
    ruby_sysinit(&argc, &argv);
    RUBY_INIT_STACK
    ruby_init();

    initBindings();

    rb_eval_string("MKXP_Z.init_game_state");
    SDL_Event e;
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);

    return 0;
}