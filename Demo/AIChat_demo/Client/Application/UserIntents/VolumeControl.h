#ifndef VOLUME_CONTROL_H
#define VOLUME_CONTROL_H

#ifdef __arm__
#include <json/json.h>
#else
#include <jsoncpp/json/json.h>
#endif

namespace VolumeControl {
    void AdjustVolume(const Json::Value& arguments);
}

#endif // VOLUME_CONTROL_H