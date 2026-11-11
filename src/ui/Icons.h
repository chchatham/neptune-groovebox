#pragma once

#include <cstdint>

namespace Icons
{

// 3x5 left arrow: ◀
static constexpr uint8_t ARROW_LEFT[] = {
    0x04, // ..#..
    0x0E, // .###.
    0x1F, // #####
};
static constexpr int ARROW_LEFT_W = 3;
static constexpr int ARROW_LEFT_H = 5;

// 3x5 right arrow: ▶
static constexpr uint8_t ARROW_RIGHT[] = {
    0x1F, // #####
    0x0E, // .###.
    0x04, // ..#..
};
static constexpr int ARROW_RIGHT_W = 3;
static constexpr int ARROW_RIGHT_H = 5;

// 5x7 play triangle
static constexpr uint8_t PLAY[] = {
    0x7F, // #######
    0x3E, // .#####.
    0x1C, // ..###..
    0x08, // ...#...
    0x00, // .......
};
static constexpr int PLAY_W = 5;
static constexpr int PLAY_H = 7;

// 5x5 stop square
static constexpr uint8_t STOP[] = {
    0x1F, // #####
    0x1F, // #####
    0x1F, // #####
    0x1F, // #####
    0x1F, // #####
};
static constexpr int STOP_W = 5;
static constexpr int STOP_H = 5;

// 5x5 record circle (approximation)
static constexpr uint8_t RECORD[] = {
    0x0E, // .###.
    0x1F, // #####
    0x1F, // #####
    0x1F, // #####
    0x0E, // .###.
};
static constexpr int RECORD_W = 5;
static constexpr int RECORD_H = 5;

} // namespace Icons
