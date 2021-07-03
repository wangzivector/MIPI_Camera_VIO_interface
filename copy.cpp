#include <SDL/SDL.h>
#include <assert.h>
#include <math.h>

/* This macro simplifies accessing a given pixel component on a surface. */
#define pel(surf, x, y, rgb) ((unsigned char *)(surf->pixels))[y*(surf->pitch)+x*3+rgb]


int main(int argc, char *argv[])
{
    int x, y, t;
    
    /* Event information is placed in here */
    SDL_Event event;
    /* This will be used as our "handle" to the screen surface */
    SDL_Surface *scr;
    SDL_Init(SDL_INIT_VIDEO);

    /* Get a 640x480, 24-bit software screen surface */
    scr = SDL_SetVideoMode(640, 480, 24, SDL_SWSURFACE);
    assert(scr);
    
    /* Ensures we have exclusive access to the pixels */
    SDL_LockSurface(scr);
    
    for(y = 0; y < scr->h; y++)
        for(x = 0; x < scr->w; x++)
        {
            /* This is what generates the pattern based on the xy co-ord */
            t = ((x*x + y*y) & 511) - 256;
            if (t < 0)
                t = -(t + 1);

            /* Now we write to the surface */
            pel(scr, x, y, 0) = 255 - t; //red
            pel(scr, x, y, 1) = t; //green
            pel(scr, x, y, 2) = t; //blue
        }
    SDL_UnlockSurface(scr);

    /* Copies the `scr' surface to the _actual_ screen */
    SDL_UpdateRect(scr, 0, 0, 0, 0);

    
    /* Now we wait for an event to arrive */
    while(SDL_WaitEvent(&event))
    {
        /* Any of these event types will end the program */
        if (event.type == SDL_QUIT
         || event.type == SDL_KEYDOWN
         || event.type == SDL_KEYUP)
            break;
    }
    SDL_Quit();
    return EXIT_SUCCESS;
}

// #include <SDL/SDL.h>
// #include <assert.h>
// #include <stdint.h>
// #include <stdlib.h>

// #define WIDTH 256
// #define HEIGHT 256

// static _Bool init_app(const char * name, SDL_Surface * icon, uint32_t flags)
// {
//     atexit(SDL_Quit);
//     if(SDL_Init(flags) < 0)
//         return 0;

//     SDL_WM_SetCaption(name, name);
//     SDL_WM_SetIcon(icon, NULL);

//     return 1;
// }

// static uint8_t * init_data(uint8_t * data)
// {
//     for(size_t i = WIDTH * HEIGHT * 3; i--; )
//         data[i] = (i % 3 == 0) ? (i / 3) % WIDTH :
//             (i % 3 == 1) ? (i / 3) / WIDTH : 0;

//     return data;
// }

// static _Bool process(uint8_t * data)
// {
//     for(SDL_Event event; SDL_PollEvent(&event);)
//         if(event.type == SDL_QUIT) return 0;

//     for(size_t i = 0; i < WIDTH * HEIGHT * 3; i += 1 + rand() % 3)
//         data[i] -= rand() % 8;

//     return 1;
// }

// static void render(SDL_Surface * sf)
// {
//     SDL_Surface * screen = SDL_GetVideoSurface();
//     if(SDL_BlitSurface(sf, NULL, screen, NULL) == 0)
//         SDL_UpdateRect(screen, 0, 0, 0, 0);
// }