//
//  AvatarMixerClientData.h
//  assignment-client/src/avatars
//
//  Created by Stephen Birarda on 2/4/2014.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AvatarMixerClientData_h
#define hifi_AvatarMixerClientData_h

#include <algorithm>
#include <cfloat>

#include <QtCore/QJsonObject>
#include <QtCore/QUrl>

#include <AvatarData.h>
#include <NodeData.h>

class AvatarMixerClientData : public NodeData {
    Q_OBJECT
public:
    AvatarMixerClientData();

    int parseData(const QByteArray& packet);
    AvatarData& getAvatar() { return _avatar; }
    
    bool checkAndSetHasReceivedFirstPackets();
    
    quint64 getBillboardChangeTimestamp() const { return _billboardChangeTimestamp; }
    void setBillboardChangeTimestamp(quint64 billboardChangeTimestamp) { _billboardChangeTimestamp = billboardChangeTimestamp; }
    
    quint64 getIdentityChangeTimestamp() const { return _identityChangeTimestamp; }
    void setIdentityChangeTimestamp(quint64 identityChangeTimestamp) { _identityChangeTimestamp = identityChangeTimestamp; }
   
    float getFullRateDistance() const { return _fullRateDistance; }
    void setMaxFullRateDistance(float distance) { _maxFullRateDistance = distance; }

    void resetNumAvatarsSentLastFrame() { _numAvatarsSentLastFrame = 0; }
    void increaseNumAvatarsSentLastFrame() { ++_numAvatarsSentLastFrame; }
    int getNumAvatarsSentLastFrame() const { return _numAvatarsSentLastFrame; }

    void loadJSONStats(QJsonObject& jsonObject) const;
private:
    AvatarData _avatar;
    bool _hasReceivedFirstPackets;
    quint64 _billboardChangeTimestamp;
    quint64 _identityChangeTimestamp;
    float _fullRateDistance = FLT_MAX;
    float _maxFullRateDistance = FLT_MAX;
    int _numAvatarsSentLastFrame = 0;
};

#endif // hifi_AvatarMixerClientData_h
