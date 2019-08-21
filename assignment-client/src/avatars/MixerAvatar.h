//
//  MixerAvatar.h
//  assignment-client/src/avatars
//
//  Created by Simon Walton Feb 2019.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

// Avatar class for use within the avatar mixer - encapsulates data required only for
// sorting priorities within the mixer.

#ifndef hifi_MixerAvatar_h
#define hifi_MixerAvatar_h

#include <AvatarData.h>

class ResourceRequest;

class MixerAvatar : public AvatarData {
    Q_OBJECT
public:
    ~MixerAvatar();
    bool getNeedsHeroCheck() const { return _needsHeroCheck; }
    void setNeedsHeroCheck(bool needsHeroCheck = true) { _needsHeroCheck = needsHeroCheck; }

    void fetchAvatarFST();
    virtual bool isCertifyFailed() const override { return _verifyState == verificationFailed; }
    bool needsIdentityUpdate() const { return _needsIdentityUpdate; }
    void setNeedsIdentityUpdate(bool value = true) { _needsIdentityUpdate = value; }

    void processCertifyEvents();
    void handleChallengeResponse(ReceivedMessage& response);

    // Avatar certification/verification:
    enum VerifyState {
        nonCertified, requestingFST, receivedFST, staticValidation, requestingOwner, ownerResponse,
        challengeClient, verified, verificationFailed, verificationSucceeded, error
    };
    Q_ENUM(VerifyState)

private:
    bool _needsHeroCheck { false };
    static const char* stateToName(VerifyState state);
    VerifyState _verifyState { nonCertified };
    std::atomic<bool> _pendingEvent { false };
    QMutex _avatarCertifyLock;
    ResourceRequest* _avatarRequest { nullptr };
    QString _marketplaceIdFromURL;
    QString _marketplaceIdFromFST;
    QByteArray _avatarFSTContents;
    QByteArray _certificateHash;
    QString _certificateIdFromURL;
    QString _certificateIdFromFST;
    QString _dynamicMarketResponse;
    QString _ownerPublicKey;
    QByteArray _challengeNonceHash;
    QTimer* _challengeTimeout { nullptr };
    bool _needsIdentityUpdate { false };

    bool generateFSTHash();
    bool validateFSTHash(const QString& publicKey) const;
    QByteArray canonicalJson(const QString fstFile);
    void sendOwnerChallenge();

    static const QString VERIFY_FAIL_MODEL;

private slots:
    void fstRequestComplete();
    void ownerRequestComplete();
};

using MixerAvatarSharedPointer = std::shared_ptr<MixerAvatar>;

#endif  // hifi_MixerAvatar_h
