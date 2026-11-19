// SPDX-License-Identifier: MIT
#include "arcade_host.h"

#include "arcade_config.h"
#include "colors.h"
#include "physics.h"
#include "render_player.h"
#include "render_road.h"
#include "render_hud.h"
#include "rendering.h"
#include "track.h"
#include "arcade_utils.h"

#include <Arduino.h>

namespace
{
bool s_live = false;
int timeOfDay = 0;
long distSinceTimeChange = 0;
}  // namespace

// Defined in rendering.cpp
extern TFT_eSprite spr;

bool arcadeHostBegin(uint16_t* fb320x240)
{
    if (s_live) return true;
    randomSeed(micros());

    if (fb320x240) spr.setBuffer(fb320x240, SCR_W, SCR_H);
    else if (!spr.created())
    {
        if (spr.createSprite(SCR_W, SCR_H) == nullptr) return false;
    }

    initPhysics();
    initColors(timeOfDay);
    initBackground();
    buildTrack();
    initTraffic(maxSpeed);

    // Skip the 3s rotating-car title; jump straight into the race.
    lastFrameMs = millis();
    distSinceTimeChange = 0;
    timeOfDay = 0;
    skyOffset = 0;
    s_live = true;
    return true;
}

void arcadeHostEnd(void)
{
    if (!s_live) return;
    setManualSteer(false, false);
    // Keep the external canvas buffer; only free owned sprites.
    // bgSpr is owned by rendering.cpp — delete via its destructor on next begin
    // if we recreate; for now leave it (PSRAM) until process end.
    s_live = false;
}

void arcadeHostSetSteer(bool left, bool right) { setManualSteer(left, right); }

bool arcadeHostTick(void)
{
    if (!s_live) return false;

    unsigned long now = millis();
    float dt = (now - lastFrameMs) / 1000.0f;
    lastFrameMs = now;
    if (dt > 0.1f) dt = 0.1f;

    if (!crashed)
    {
        handleInput(dt);
        updatePhysics(dt);
        checkCollisions();

        int pSeg = findSegIdx(position + playerZdist);
        float curveForce = segments[pSeg].curve;
        skyOffset += curveForce * (speed / maxSpeed) * 150.0f * dt;
    }

    drawSky(position, playerZdist, timeOfDay, skyOffset);
    drawRoad(position, playerX, playerZdist, cameraDepth, timeOfDay);
    drawPlayerCar();
    drawHUD(speed, maxSpeed, currentLapTime, bestLapTime);

    if (crashed)
    {
        drawCrashMessage();
        if (millis() - crashTimer > 2000)
        {
            crashed = false;
            speed = 0;
            playerX = 0;
        }
    }

    distSinceTimeChange += (long)(speed * dt);
    if (distSinceTimeChange > 180000)
    {
        distSinceTimeChange = 0;
        timeOfDay = (timeOfDay + 1) % 3;
        initColors(timeOfDay);
    }
    return true;
}

uint16_t* arcadeHostFramebuffer(void) { return spr.buffer(); }
