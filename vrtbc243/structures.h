#pragma once

#undef UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "stCommon.h"

// Resolves the model's key-bone identifiers and builds both direct and
// transitive parent/child tables for optional character pose adjustments.
struct BoneNameLookup
{
    std::vector<std::pair<std::string, int>> boneNames;
    std::map<int, std::vector<int>> boneLayout;
    std::map<int, std::vector<int>> allChildren;
    std::vector<int> parentList;
    std::vector<int> keyBoneList;
    int oldBoneOffset = 0;

    BoneNameLookup()
    {
        static const char* const names[] = {
            "ArmL", "ArmR", "ShoulderL", "ShoulderR", "SpineLow", "Waist",
            "Head", "Jaw", "IndexFingerR", "MiddleFingerR", "PinkyFingerR",
            "RingFingerR", "ThumbR", "IndexFingerL", "MiddleFingerL",
            "PinkyFingerL", "RingFingerL", "ThumbL", "$BTH", "$CSR", "$CSL",
            "_Breath", "_Name", "_NameMount", "$CHD", "$CCH", "Root",
            "Wheel1", "Wheel2", "Wheel3", "Wheel4", "Wheel5", "Wheel6",
            "Wheel7", "Wheel8", "HandL", "HandR", "ElbowL", "ElbowR"
        };
        boneNames.reserve(sizeof(names) / sizeof(names[0]));
        for (const char* name : names) boneNames.emplace_back(name, -1);
    }

    std::string Get(int boneIndex)
    {
        return boneIndex >= 0 && boneIndex < static_cast<int>(boneNames.size())
            ? boneNames[boneIndex].first : "_NA_";
    }

    int Get(std::string name)
    {
        for (const auto& bone : boneNames)
            if (bone.first == name) return bone.second;
        return -1;
    }

    bool Set(int boneCount, int boneOffset)
    {
        if (oldBoneOffset == boneOffset) return false;
        oldBoneOffset = boneOffset;

        for (auto& bone : boneNames) bone.second = -1;
        boneLayout.clear();
        allChildren.clear();
        parentList.assign(boneCount, 0);
        keyBoneList.assign(boneCount, 0);

        for (int index = 0; index < boneCount; ++index)
        {
            const int record = boneOffset + index * 0x58;
            const int keyBone = *reinterpret_cast<int*>(record);
            const short parent = *reinterpret_cast<short*>(record + 0x08);
            if (keyBone >= 0 && keyBone < static_cast<int>(boneNames.size()))
                boneNames[keyBone].second = index;
            parentList[index] = parent;
            keyBoneList[index] = keyBone;
            boneLayout[parent].push_back(index);
            boneLayout[index];
        }

        const int handLName = NameIndex("HandL");
        const int handRName = NameIndex("HandR");
        const int elbowLName = NameIndex("ElbowL");
        const int elbowRName = NameIndex("ElbowR");
        PopulateLimb("ThumbL", handLName, elbowLName);
        PopulateLimb("ThumbR", handRName, elbowRName);

        UpdateChildren(-1);
        return true;
    }

    std::unordered_set<int> UpdateChildren(int index)
    {
        std::unordered_set<int> descendants;
        const auto direct = boneLayout.find(index);
        if (direct != boneLayout.end())
        {
            for (int child : direct->second)
            {
                descendants.insert(child);
                const std::unordered_set<int> nested = UpdateChildren(child);
                descendants.insert(nested.begin(), nested.end());
            }
        }
        std::vector<int>& ordered = allChildren[index];
        ordered.assign(descendants.begin(), descendants.end());
        std::sort(ordered.begin(), ordered.end());
        return descendants;
    }

private:
    int NameIndex(const char* name) const
    {
        for (size_t index = 0; index < boneNames.size(); ++index)
            if (boneNames[index].first == name) return static_cast<int>(index);
        return -1;
    }

    void PopulateLimb(const char* thumbName, int handName, int elbowName)
    {
        const int thumb = Get(thumbName);
        if (thumb < 0 || thumb >= static_cast<int>(parentList.size())) return;
        const int hand = parentList[thumb];
        if (handName < 0 || hand < 0 || hand >= static_cast<int>(keyBoneList.size())) return;
        boneNames[handName].second = hand;
        keyBoneList[hand] = handName;
        const int elbow = parentList[hand];
        if (elbowName < 0 || elbow < 0 || elbow >= static_cast<int>(keyBoneList.size())) return;
        boneNames[elbowName].second = elbow;
        keyBoneList[elbow] = elbowName;
    }
};

BoneNameLookup boneLookup;

// These are 32-bit views over game-owned memory. Packing and field order are
// intentional; the assertions below guard every offset consumed by the hooks.
#pragma pack(push, 1)

struct st20Container
{
    char unknown1[0x0150];
    int ptr20;
    char unknown2[0x001C];
    int ptrSKIN;
};

struct stModelContainer
{
    char unknown1[0x002C];
    st20Container* p20Container;
    char unknown2[0x0064];
    int ptrBoneData;
    int ptrBonePos;
    char unknown3[0x0018];
    uMatrix characterMatrix;
    uMatrix cameraMatrix;
    uMatrix Matrix3;
};

struct stObjectData
{
    int unknown1;
    int unknown2;
    int unknown3;
    int unknown4;
    Vector3 objPos;
    float unknown5;
    float objRot;
    float objPitch;
    int unknown6;
    int unknown7;
    int Animation1;
    int Animation2;
    float unknown8;
    float unknown9;
    float unknown10;
    int MovementStatus;
    int unknown11;
    Vector3 objPos1;
    float objRot1;
    float objPitch1;
    int tickAtMovement;
    Vector3 forwardVectorX;
    Vector3 upVector;
    float unknown12;
    float unknown13;
    float unknown14;
    float unknown15;
    float curSpeed;
    float walkSpeedForward;
    float runSpeedForward;
    float runSpeedBackward;
    float swimSpeedForward;
    float swimSpeedBackward;
    float flySpeedForward;
    float flySpeedBackward;
};

struct stObjectManager
{
    char unknown0[0x0028];
    stObjectManager* nextInLink;
    int unknown1;
    int objGuID;
    int unknown2;
    int unknown3;
    stObjectManager* nextInLink1;
    char unknown4[0x0058];
    float scale1;
    float scale2;
    char unknown5[0x0014];
    stModelContainer* pModelContainer;
    char unknown6[0x10];
    byte alpha1;
    byte alpha2;
    byte alpha3;
    byte alpha4;
    char unknown7[0x0C];
    stObjectData* ptrObjectData;
    char unknown8[0x06AC];
    stObjectData objectData;
    char unknown9[0x018C];
    int mount;
    char unknown10[0x0644];
    int unknown11;
    char unknown12[0x0A58];
    int modelID;
};

#pragma pack(pop)

static_assert(offsetof(st20Container, ptr20) == 0x150, "st20Container::ptr20 offset changed");
static_assert(offsetof(stModelContainer, p20Container) == 0x2C, "stModelContainer::p20Container offset changed");
static_assert(offsetof(stModelContainer, ptrBonePos) == 0x98, "stModelContainer::ptrBonePos offset changed");
static_assert(offsetof(stObjectData, objPos) == 0x10, "stObjectData::objPos offset changed");
static_assert(offsetof(stObjectData, MovementStatus) == 0x44, "stObjectData::MovementStatus offset changed");
static_assert(offsetof(stObjectManager, pModelContainer) == 0xB4, "stObjectManager::pModelContainer offset changed");
static_assert(offsetof(stObjectManager, ptrObjectData) == 0xD8, "stObjectManager::ptrObjectData offset changed");
static_assert(offsetof(stObjectManager, objectData) == 0x788, "stObjectManager::objectData offset changed");
static_assert(offsetof(stObjectManager, mount) == 0x9C0, "stObjectManager::mount offset changed");
static_assert(offsetof(stObjectManager, modelID) == 0x1A64, "stObjectManager::modelID offset changed");
