//
//  EntityCollisionSystem.cpp
//  libraries/entities/src
//
//  Created by Brad Hefta-Gaub on 9/23/14.
//  Copyright 2013-2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <algorithm>

#include <AbstractAudioInterface.h>
#include <AvatarHashMap.h>
#include <CollisionInfo.h>
#include <HeadData.h>
#include <HandData.h>
#include <PerfStat.h>
#include <SphereShape.h>

#include "EntityCollisionSystem.h"
#include "EntityEditPacketSender.h"
#include "EntityItem.h"
#include "EntityTreeElement.h"
#include "EntityTree.h"

const int MAX_COLLISIONS_PER_Entity = 16;

EntityCollisionSystem::EntityCollisionSystem()
    :   SimpleEntitySimulation(), 
        _packetSender(NULL),
        _collisions(MAX_COLLISIONS_PER_Entity) {
}

void EntityCollisionSystem::init(EntityEditPacketSender* packetSender,
        EntityTree* entities) {
    assert(entities);
    setEntityTree(entities);
    _packetSender = packetSender;
}

EntityCollisionSystem::~EntityCollisionSystem() {
}

void EntityCollisionSystem::updateCollisions() {
    PerformanceTimer perfTimer("collisions");
    assert(_entityTree);
    // update all Entities
    if (_entityTree->tryLockForWrite()) {
        foreach (EntityItem* entity, _movingEntities) {
            checkEntity(entity);
        }
        _entityTree->unlock();
    }
}


void EntityCollisionSystem::checkEntity(EntityItem* entity) {
    updateCollisionWithEntities(entity);
    updateCollisionWithAvatars(entity);
}

void EntityCollisionSystem::emitGlobalEntityCollisionWithEntity(EntityItem* entityA, 
                                            EntityItem* entityB, const Collision& collision) {
                                            
    EntityItemID idA = entityA->getEntityItemID();
    EntityItemID idB = entityB->getEntityItemID();
    emit entityCollisionWithEntity(idA, idB, collision);
}

void EntityCollisionSystem::updateCollisionWithEntities(EntityItem* entityA) {

    if (entityA->getIgnoreForCollisions()) {
        return; // bail early if this entity is to be ignored...
    }

    // don't collide entities with unknown IDs,
    if (!entityA->isKnownID()) {
        return;
    }

    glm::vec3 penetration;
    EntityItem* entityB = NULL;
    
    const int MAX_COLLISIONS_PER_ENTITY = 32;
    CollisionList collisions(MAX_COLLISIONS_PER_ENTITY);
    bool shapeCollisionsAccurate = false;
    
    bool shapeCollisions = _entityTree->findShapeCollisions(&entityA->getCollisionShapeInMeters(), 
                                            collisions, Octree::NoLock, &shapeCollisionsAccurate);

    if (shapeCollisions) {
        for(int i = 0; i < collisions.size(); i++) {

            CollisionInfo* collision = collisions[i];
            penetration = collision->_penetration;
            entityB = static_cast<EntityItem*>(collision->_extraData);
            
            // The collision _extraData should be a valid entity, but if for some reason
            // it's NULL then continue with a warning.
            if (!entityB) {
                qDebug() << "UNEXPECTED - we have a collision with missing _extraData. Something went wrong down below!";
                continue; // skip this loop pass if the entity is NULL
            }
            
            // don't collide entities with unknown IDs,
            if (!entityB->isKnownID()) {
                continue; // skip this loop pass if the entity has an unknown ID
            }
            
            // NOTE: 'penetration' is the depth that 'entityA' overlaps 'entityB'.  It points from A into B.
            glm::vec3 penetrationInTreeUnits = penetration / (float)(TREE_SCALE);

            // Even if the Entities overlap... when the Entities are already moving appart
            // we don't want to count this as a collision.
            glm::vec3 relativeVelocity = entityA->getVelocity() - entityB->getVelocity();
            
            bool fullyEnclosedCollision = glm::length(penetrationInTreeUnits) > entityA->getLargestDimension();

            bool wantToMoveA = entityA->getCollisionsWillMove();
            bool wantToMoveB = entityB->getCollisionsWillMove();
            bool movingTowardEachOther = glm::dot(relativeVelocity, penetrationInTreeUnits) > 0.0f;

            // only do collisions if the entities are moving toward each other and one or the other
            // of the entities are movable from collisions
            bool doCollisions = !fullyEnclosedCollision && movingTowardEachOther && (wantToMoveA || wantToMoveB); 

            if (doCollisions) {

                quint64 now = usecTimestampNow();
        
                glm::vec3 axis = glm::normalize(penetration);
                glm::vec3 axialVelocity = glm::dot(relativeVelocity, axis) * axis;

                float massA = entityA->computeMass();
                float massB = entityB->computeMass();
                float totalMass = massA + massB;
                float massRatioA = (2.0f * massB / totalMass);
                float massRatioB = (2.0f * massA / totalMass);

                // in the event that one of our entities is non-moving, then fix up these ratios
                if (wantToMoveA && !wantToMoveB) {
                    massRatioA = 2.0f;
                    massRatioB = 0.0f;
                }

                if (!wantToMoveA && wantToMoveB) {
                    massRatioA = 0.0f;
                    massRatioB = 2.0f;
                }
            
                // unless the entity is configured to not be moved by collision, calculate it's new position
                // and velocity and apply it
                if (wantToMoveA) {
                    // handle Entity A
                    glm::vec3 newVelocityA = entityA->getVelocity() - axialVelocity * massRatioA;
                    glm::vec3 newPositionA = entityA->getPosition() - 0.5f * penetrationInTreeUnits;

                    EntityItemProperties propertiesA = entityA->getProperties();
                    EntityItemID idA(entityA->getID());
                    propertiesA.setVelocity(newVelocityA * (float)TREE_SCALE);
                    propertiesA.setPosition(newPositionA * (float)TREE_SCALE);
                    propertiesA.setLastEdited(now);

                    // NOTE: EntityTree::updateEntity() will cause the entity to get sorted correctly in the EntitySimulation,
                    // thereby waking up static non-moving entities.
                    _entityTree->updateEntity(entityA, propertiesA);
                    _packetSender->queueEditEntityMessage(PacketTypeEntityAddOrEdit, idA, propertiesA);
                }            

                // unless the entity is configured to not be moved by collision, calculate it's new position
                // and velocity and apply it
                if (wantToMoveB) {
                    glm::vec3 newVelocityB = entityB->getVelocity() + axialVelocity * massRatioB;
                    glm::vec3 newPositionB = entityB->getPosition() + 0.5f * penetrationInTreeUnits;

                    EntityItemProperties propertiesB = entityB->getProperties();

                    EntityItemID idB(entityB->getID());
                    propertiesB.setVelocity(newVelocityB * (float)TREE_SCALE);
                    propertiesB.setPosition(newPositionB * (float)TREE_SCALE);
                    propertiesB.setLastEdited(now);

                    // NOTE: EntityTree::updateEntity() will cause the entity to get sorted correctly in the EntitySimulation,
                    // thereby waking up static non-moving entities.
                    _entityTree->updateEntity(entityB, propertiesB);
                    _packetSender->queueEditEntityMessage(PacketTypeEntityAddOrEdit, idB, propertiesB);
                }

                // NOTE: Do this after updating the entities so that the callback can delete the entities if they want to
                Collision collision;
                collision.penetration = penetration;
                collision.contactPoint = (0.5f * (float)TREE_SCALE) * (entityA->getPosition() + entityB->getPosition());
                emitGlobalEntityCollisionWithEntity(entityA, entityB, collision);
            }
        }
    }
}

void EntityCollisionSystem::updateCollisionWithAvatars(EntityItem* entity) {
    
    // Entities that are in hand, don't collide with avatars
    if (!DependencyManager::get<AvatarHashMap>()) {
        return;
    }

    if (entity->getIgnoreForCollisions() || !entity->getCollisionsWillMove()) {
        return; // bail early if this entity is to be ignored or wont move
    }

    glm::vec3 center = entity->getPosition() * (float)(TREE_SCALE);
    float radius = entity->getRadius() * (float)(TREE_SCALE);
    const float ELASTICITY = 0.9f;
    const float DAMPING = 0.1f;
    glm::vec3 penetration;

    _collisions.clear();
    foreach (const AvatarSharedPointer& avatarPointer, DependencyManager::get<AvatarHashMap>()->getAvatarHash()) {
        AvatarData* avatar = avatarPointer.data();

        float totalRadius = avatar->getBoundingRadius() + radius;
        glm::vec3 relativePosition = center - avatar->getPosition();
        if (glm::dot(relativePosition, relativePosition) > (totalRadius * totalRadius)) {
            continue;
        }

        if (avatar->findSphereCollisions(center, radius, _collisions)) {
            int numCollisions = _collisions.size();
            for (int i = 0; i < numCollisions; ++i) {
                CollisionInfo* collision = _collisions.getCollision(i);
                collision->_damping = DAMPING;
                collision->_elasticity = ELASTICITY;
    
                collision->_addedVelocity /= (float)(TREE_SCALE);
                glm::vec3 relativeVelocity = collision->_addedVelocity - entity->getVelocity();
    
                if (glm::dot(relativeVelocity, collision->_penetration) <= 0.0f) {
                    // only collide when Entity and collision point are moving toward each other
                    // (doing this prevents some "collision snagging" when Entity penetrates the object)
                    collision->_penetration /= (float)(TREE_SCALE);
                    applyHardCollision(entity, *collision);
                }
            }
        }
    }
}

void EntityCollisionSystem::applyHardCollision(EntityItem* entity, const CollisionInfo& collisionInfo) {

    // don't collide entities with unknown IDs,
    if (!entity->isKnownID()) {
        return;
    }

    // HALTING_* params are determined using expected acceleration of gravity over some timescale.  
    // This is a HACK for entities that bounce in a 1.0 gravitational field and should eventually be made more universal.
    const float HALTING_ENTITY_PERIOD = 0.0167f;  // ~1/60th of a second
    const float HALTING_ENTITY_SPEED = 9.8 * HALTING_ENTITY_PERIOD / (float)(TREE_SCALE);

    //
    //  Update the entity in response to a hard collision.  Position will be reset exactly
    //  to outside the colliding surface.  Velocity will be modified according to elasticity.
    //
    //  if elasticity = 0.0, collision is inelastic (vel normal to collision is lost)
    //  if elasticity = 1.0, collision is 100% elastic.
    //
    glm::vec3 position = entity->getPosition();
    glm::vec3 velocity = entity->getVelocity();

    const float EPSILON = 0.0f;
    glm::vec3 relativeVelocity = collisionInfo._addedVelocity - velocity;
    float velocityDotPenetration = glm::dot(relativeVelocity, collisionInfo._penetration);
    if (velocityDotPenetration < EPSILON) {
        // entity is moving into collision surface
        //
        // TODO: do something smarter here by comparing the mass of the entity vs that of the other thing 
        // (other's mass could be stored in the Collision Info).  The smaller mass should surrender more 
        // position offset and should slave more to the other's velocity in the static-friction case.
        position -= collisionInfo._penetration;

        if (glm::length(relativeVelocity) < HALTING_ENTITY_SPEED) {
            // static friction kicks in and entities moves with colliding object
            velocity = collisionInfo._addedVelocity;
        } else {
            glm::vec3 direction = glm::normalize(collisionInfo._penetration);
            velocity += glm::dot(relativeVelocity, direction) * (1.0f + collisionInfo._elasticity) * direction;    // dynamic reflection
            velocity += glm::clamp(collisionInfo._damping, 0.0f, 1.0f) * (relativeVelocity - glm::dot(relativeVelocity, direction) * direction);   // dynamic friction
        }
    }

    EntityItemProperties properties = entity->getProperties();
    EntityItemID entityItemID(entity->getID());

    properties.setPosition(position * (float)TREE_SCALE);
    properties.setVelocity(velocity * (float)TREE_SCALE);
    properties.setLastEdited(usecTimestampNow());

    _entityTree->updateEntity(entity, properties);
    _packetSender->queueEditEntityMessage(PacketTypeEntityAddOrEdit, entityItemID, properties);
}
