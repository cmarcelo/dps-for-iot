/*
 *******************************************************************
 *
 * Copyright 2016 Intel Corporation All rights reserved.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 */

#include <assert.h>
#include <string.h>
#include <malloc.h>
#include <uv.h>
#include <dps/dbg.h>
#include <dps/dps.h>
#include <dps/uuid.h>
#include <dps/private/dps.h>
#include <dps/private/network.h>
#include "node.h"
#include "bitvec.h"
#include <dps/private/cbor.h>
#include "sub.h"
#include "pub.h"
#include "topics.h"
#include "node.h"
#include "compat.h"

/*
 * Debug control for this module
 */
DPS_DEBUG_CONTROL(DPS_DEBUG_ON);

#define DESCRIBE(n)  DPS_NodeAddrToString(&(n)->ep.addr)

static int IsValidSub(const DPS_Subscription* sub)
{
    DPS_Subscription* subList;

    if (!sub || !sub->node || !sub->node->loop) {
        return DPS_FALSE;
    }
    DPS_LockNode(sub->node);
    for (subList = sub->node->subscriptions; subList != NULL; subList = subList->next) {
        if (sub == subList) {
            break;
        }
    }
    DPS_UnlockNode(sub->node);
    return subList != NULL;
}

size_t DPS_SubscriptionGetNumTopics(const DPS_Subscription* sub)
{
    return IsValidSub(sub) ? sub->numTopics : 0;
}

const char* DPS_SubscriptionGetTopic(const DPS_Subscription* sub, size_t index)
{
    if (IsValidSub(sub) && (sub->numTopics > index)) {
        return sub->topics[index];
    } else {
        return NULL;
    }
}

DPS_Node* DPS_SubscriptionGetNode(const DPS_Subscription* sub)
{
    if (IsValidSub(sub)) {
        return sub->node;
    } else {
        return NULL;
    }
}

static DPS_Subscription* FreeSubscription(DPS_Subscription* sub)
{
    DPS_Subscription* next = sub->next;
    DPS_BitVectorFree(sub->bf);
    DPS_BitVectorFree(sub->needs);
    while (sub->numTopics) {
        free(sub->topics[--sub->numTopics]);
    }
    free(sub);
    return next;
}

void DPS_FreeSubscriptions(DPS_Node* node)
{
    while (node->subscriptions) {
        node->subscriptions = FreeSubscription(node->subscriptions);
    }
}

DPS_Subscription* DPS_CreateSubscription(DPS_Node* node, const char** topics, size_t numTopics)
{
    size_t i;
    DPS_Subscription* sub;

    if (!node || !topics || !numTopics) {
        return NULL;
    }
    sub = calloc(1, sizeof(DPS_Subscription) + sizeof(char*) * (numTopics - 1));
    /*
     * Add the topics to the subscription
     */
    for (i = 0; i < numTopics; ++i) {
        sub->topics[i] = strndup(topics[i], DPS_MAX_TOPIC_STRLEN);
        if (!sub->topics[i]) {
            FreeSubscription(sub);
            return NULL;
        }
        ++sub->numTopics;
    }
    sub->node = node;
    return sub;
}

DPS_Status DPS_DestroySubscription(DPS_Subscription* sub)
{
    DPS_Node* node;

    if (!IsValidSub(sub)) {
        return DPS_ERR_MISSING;
    }
    node = sub->node;
    /*
     * Protect the node while we update it
     */
    DPS_LockNode(node);
    /*
     * Unlink the subscription
     */
    if (node->subscriptions == sub) {
        node->subscriptions = sub->next;
    } else {
        DPS_Subscription* prev = node->subscriptions;
        while (prev->next != sub) {
            prev = prev->next;
        }
        prev->next = sub->next;
    }
    /*
     * This removes this subscription's contributions to the interests and needs
     */
    if (DPS_CountVectorDel(node->interests, sub->bf) != DPS_OK) {
        assert(!"Count error");
    }
    if (DPS_CountVectorDel(node->needs, sub->needs) != DPS_OK) {
        assert(!"Count error");
    }
    DPS_UnlockNode(node);

    DPS_DBGPRINT("Unsubscribing from %zu topics\n", sub->numTopics);
    FreeSubscription(sub);

    DPS_UpdateSubs(node, NULL);

    return DPS_OK;
}

DPS_Status DPS_SendSubscription(DPS_Node* node, RemoteNode* remote, DPS_BitVector* interests)
{
    DPS_Status ret;
    DPS_TxBuffer buf;
    size_t len;

    if (!node->netCtx) {
        return DPS_ERR_NETWORK;
    }
    /*
     * Subscription is encoded as an array of 3 elements
     *  [
     *      type,
     *      { headers },
     *      { body }
     *  ]
     */
    len = CBOR_SIZEOF_ARRAY(3) +
          CBOR_SIZEOF(uint8_t) +

          CBOR_SIZEOF_MAP(1) + 2 * CBOR_SIZEOF(uint8_t) +
          CBOR_SIZEOF(uint16_t) +
          CBOR_SIZEOF_BSTR(sizeof(DPS_UUID));

    if (interests) {
        len += CBOR_SIZEOF_MAP(4) + 4 * CBOR_SIZEOF(uint8_t) +
               CBOR_SIZEOF_BOOL +
               CBOR_SIZEOF_BOOL +
               DPS_BitVectorSerializeMaxSize(interests) +
               DPS_BitVectorSerializeMaxSize(remote->outbound.needs);
    } else {
        len += CBOR_SIZEOF_MAP(0);
    }

    ret = DPS_TxBufferInit(&buf, NULL, len);
    if (ret == DPS_OK) {
        ret = CBOR_EncodeArray(&buf, 3);
    }
    if (ret == DPS_OK) {
        ret = CBOR_EncodeUint8(&buf, DPS_MSG_TYPE_SUB);
    }
    /*
     * Header map
     *  {
     *      port: uint
     *      meshId : uuid
     *  }
     */
    if (ret == DPS_OK) {
        ret = CBOR_EncodeMap(&buf, 2);
    }
    if (ret == DPS_OK) {
        ret = CBOR_EncodeUint8(&buf, DPS_CBOR_KEY_PORT);
    }
    if (ret == DPS_OK) {
        ret = CBOR_EncodeInt16(&buf, node->port);
    }
    if (ret == DPS_OK) {
        ret = CBOR_EncodeUint8(&buf, DPS_CBOR_KEY_MESH_ID);
    }
    if (ret == DPS_OK) {
        ret = CBOR_EncodeBytes(&buf, (uint8_t*)&remote->outbound.meshId, sizeof(DPS_UUID));
    }
    if (ret != DPS_OK) {
        return ret;
    }
    /*
     * Body map
     *      {
     *          sequence_num: uint,
     *          inbound_sync: bool,
     *          outbound_sync: bool,
     *          needs: bit-vector,
     *          interests: bit-vector
     *       }
     * or
     *       { }
     */
    if (interests) {
        ret = CBOR_EncodeMap(&buf, 5);
        if (ret == DPS_OK) {
            ret = CBOR_EncodeUint8(&buf, DPS_CBOR_KEY_SEQ_NUM);
        }
        if (ret == DPS_OK) {
            ++remote->outbound.sequenceNum;
            ret = CBOR_EncodeUint32(&buf, remote->outbound.sequenceNum);
        }
        if (ret == DPS_OK) {
            ret = CBOR_EncodeUint8(&buf, DPS_CBOR_KEY_INBOUND_SYNC);
        }
        if (ret == DPS_OK) {
            ret = CBOR_EncodeBoolean(&buf, remote->inbound.sync);
        }
        if (ret == DPS_OK) {
            ret = CBOR_EncodeUint8(&buf, DPS_CBOR_KEY_OUTBOUND_SYNC);
        }
        if (ret == DPS_OK) {
            ret = CBOR_EncodeBoolean(&buf, remote->outbound.sync);
        }
        if (ret == DPS_OK) {
            ret = CBOR_EncodeUint8(&buf, DPS_CBOR_KEY_NEEDS);
        }
        if (ret == DPS_OK) {
            ret = DPS_BitVectorSerialize(remote->outbound.needs, &buf);
        }
        if (ret == DPS_OK) {
            ret = CBOR_EncodeUint8(&buf, DPS_CBOR_KEY_INTERESTS);
        }
        if (ret == DPS_OK) {
            ret = DPS_BitVectorSerialize(interests, &buf);
        }
    } else {
        ret = CBOR_EncodeMap(&buf, 0);
    }

#ifdef SIMULATE_LOST_SUBSCRIPTION
    /*
     * Enable this code to simulate lost subscriptions to test
     * out the resynchronization code.
     */
    if (!remote->outbound.sync && (DPS_Rand() & 0x8)) {
        DPS_ERRPRINT("Simulating lost subscription\n");
        DPS_TxBufferFree(&buf);
        remote->inbound.sync = DPS_FALSE;
        remote->outbound.sync = DPS_FALSE;
        return DPS_OK;
    }
#endif

    if (ret == DPS_OK) {
        uv_buf_t uvBuf = uv_buf_init((char*)buf.base, DPS_TxBufferUsed(&buf));
        CBOR_Dump("Sub out", (uint8_t*)uvBuf.base, uvBuf.len);
        ret = DPS_NetSend(node, NULL, &remote->ep, &uvBuf, 1, DPS_OnSendComplete);
        if (ret != DPS_OK) {
            DPS_ERRPRINT("Failed to send subscription request %s\n", DPS_ErrTxt(ret));
            DPS_SendFailed(node, &remote->ep.addr, &uvBuf, 1, ret);
        }
    } else {
        DPS_TxBufferFree(&buf);
    }
    /*
     * Done with these flags (even in case of failure)
     */
    remote->inbound.sync = DPS_FALSE;
    remote->outbound.sync = DPS_FALSE;
    return ret;
}

/*
 * Update the interests for a remote node
 */
static DPS_Status UpdateInboundInterests(DPS_Node* node, RemoteNode* remote, DPS_BitVector* interests, DPS_BitVector* needs, int delta)
{
    DPS_DBGTRACE();

    /*
     * If the remote is muted or being muted we discard incoming interests
     */
    if (remote->muted) {
        /*
         * Tie-breaker for links that are muted on both sides:
         *
         * Loop detection is independent of which endpoint initiated the link
         * but there is a race condition where both endpoints simultaneousy
         * detect the loop. Fortunately we can detect this case because each
         * endpoint sends a subscription with MaxMeshId and break the tie
         * by unmuting one end. We arbitrarily choose to unmute the endpoint
         * that originally initiated the link.
         */
        if (remote->linked && (DPS_UUIDCompare(&remote->inbound.meshId, &DPS_MaxMeshId) == 0)) {
            DPS_DBGPRINT("Unmuting %s\n", DESCRIBE(remote));
            remote->muted = DPS_REMOTE_UNMUTED;
        }
        DPS_ClearInboundInterests(node, remote);
        DPS_BitVectorFree(interests);
        DPS_BitVectorFree(needs);
        return DPS_OK;
    }
    if (remote->inbound.interests) {
        if (delta) {
            DPS_DBGPRINT("Received interests delta\n");
            DPS_BitVectorXor(interests, interests, remote->inbound.interests, NULL);
        }
        DPS_ClearInboundInterests(node, remote);
    }
    if (DPS_BitVectorIsClear(interests)) {
        DPS_BitVectorFree(interests);
        DPS_BitVectorFree(needs);
    } else {
        DPS_CountVectorAdd(node->interests, interests);
        DPS_CountVectorAdd(node->needs, needs);
        remote->inbound.interests = interests;
        remote->inbound.needs = needs;
    }

#ifndef NDEBUG
    DPS_DBGPRINT("New inbound interests from %s: ", DESCRIBE(remote));
    DPS_DumpMatchingTopics(remote->inbound.interests);
#endif

    return DPS_OK;
}

/*
 *
 */
DPS_Status DPS_DecodeSubscription(DPS_Node* node, DPS_NetEndpoint* ep, DPS_RxBuffer* buffer)
{
    static const int32_t HeaderKeys[] = { DPS_CBOR_KEY_PORT, DPS_CBOR_KEY_MESH_ID };
    static const int32_t BodyKeys[] = { DPS_CBOR_KEY_SEQ_NUM, DPS_CBOR_KEY_INBOUND_SYNC, DPS_CBOR_KEY_OUTBOUND_SYNC,
                                        DPS_CBOR_KEY_NEEDS, DPS_CBOR_KEY_INTERESTS };
    DPS_Status ret;
    DPS_BitVector* interests = NULL;
    DPS_BitVector* needs = NULL;
    uint16_t port;
    uint32_t sequenceNum = 0;
    RemoteNode* remote = NULL;
    CBOR_MapState mapState;
    DPS_UUID* meshId = NULL;
    int syncRequested = DPS_FALSE;
    int syncReceived = DPS_FALSE;

    DPS_DBGTRACE();

    CBOR_Dump("Sub in", buffer->rxPos, DPS_RxBufferAvail(buffer));
    /*
     * Parse keys from header map
     */
    ret = DPS_ParseMapInit(&mapState, buffer, HeaderKeys, A_SIZEOF(HeaderKeys));
    if (ret != DPS_OK) {
        return ret;
    }
    while (!DPS_ParseMapDone(&mapState)) {
        size_t len;
        int32_t key;
        ret = DPS_ParseMapNext(&mapState, &key);
        if (ret != DPS_OK) {
            break;
        }
        switch (key) {
        case DPS_CBOR_KEY_PORT:
            ret = CBOR_DecodeUint16(buffer, &port);
            break;
        case DPS_CBOR_KEY_MESH_ID:
            ret = CBOR_DecodeBytes(buffer, (uint8_t**)&meshId, &len);
            if ((ret == DPS_OK) && (len != sizeof(DPS_UUID))) {
                ret = DPS_ERR_INVALID;
            }
            break;
        }
        if (ret != DPS_OK) {
            break;
        }
    }
    if (ret != DPS_OK) {
        return ret;
    }
    /*
     * Parse keys from body map
     */
    ret = DPS_ParseMapInit(&mapState, buffer, BodyKeys, A_SIZEOF(BodyKeys));
    if (ret != DPS_OK) {
        return ret;
    }
    DPS_EndpointSetPort(ep, port);
    /*
     * If the body map is empty this mean the remote has asked to unlink
     */
    if (mapState.entries == 0) {
        DPS_LockNode(node);
        remote = DPS_LookupRemoteNode(node, &ep->addr);
        if (remote) {
            DPS_DeleteRemoteNode(node, remote);
            DPS_UnlockNode(node);
            DPS_UpdateSubs(node, NULL);
        } else {
            DPS_UnlockNode(node);
        }
        return DPS_OK;
    }
    /*
     * Parse out the body fields
     */
    while (!DPS_ParseMapDone(&mapState)) {
        int32_t key;
        ret = DPS_ParseMapNext(&mapState, &key);
        if (ret != DPS_OK) {
            break;
        }
        switch (key) {
        case DPS_CBOR_KEY_SEQ_NUM:
            ret = CBOR_DecodeUint32(buffer, &sequenceNum);
            break;
        case DPS_CBOR_KEY_INBOUND_SYNC:
            ret = CBOR_DecodeBoolean(buffer, &syncRequested);
            break;
        case DPS_CBOR_KEY_OUTBOUND_SYNC:
            ret = CBOR_DecodeBoolean(buffer, &syncReceived);
            break;
        case DPS_CBOR_KEY_INTERESTS:
            if (interests) {
                ret = DPS_ERR_INVALID;
            } else {
                interests = DPS_BitVectorAlloc();
                if (interests) {
                    ret = DPS_BitVectorDeserialize(interests, buffer);
                } else {
                    ret = DPS_ERR_RESOURCES;
                }
            }
            break;
        case DPS_CBOR_KEY_NEEDS:
            if (needs) {
                ret = DPS_ERR_INVALID;
            } else {
                needs = DPS_BitVectorAllocFH();
                if (needs) {
                    ret = DPS_BitVectorDeserialize(needs, buffer);
                } else {
                    ret = DPS_ERR_RESOURCES;
                }
            }
            break;
        }
        if (ret != DPS_OK) {
            break;
        }
    }
    if (ret == DPS_OK) {
        DPS_LockNode(node);
        ret = DPS_AddRemoteNode(node, &ep->addr, ep->cn, &remote);
        if (ret == DPS_ERR_EXISTS) {
            ret = DPS_OK;
        } else {
            /*
             * The remote is new to us so we need to sync
             */
            syncRequested = DPS_TRUE;
        }
        DPS_UnlockNode(node);
    }
    if (ret != DPS_OK) {
        DPS_BitVectorFree(interests);
        DPS_BitVectorFree(needs);
        return ret;
    }
    DPS_LockNode(node);
    if (syncRequested) {
        remote->outbound.sync = DPS_TRUE;
    }
    /*
     * Check the sequence number to detect lost packets
     */
    if (sequenceNum != (remote->inbound.sequenceNum + 1)) {
        DPS_ERRPRINT("Mismatched sequence number %d != %d from %s\n", sequenceNum, remote->inbound.sequenceNum + 1, DESCRIBE(remote));
        if (syncReceived) {
            if (sequenceNum > remote->inbound.sequenceNum) {
                remote->inbound.sequenceNum = sequenceNum;
            }
        } else {
            DPS_ERRPRINT("Ignore invalid delta and request resync\n");
            remote->inbound.sync = DPS_TRUE;
        }
    } else {
        ++remote->inbound.sequenceNum;
    }
    if (sequenceNum == remote->inbound.sequenceNum) {
        DPS_DBGPRINT("Received mesh id %08x from %s\n", UUID_32(meshId), DESCRIBE(remote));
        /*
         * Check for a loop in the mesh. If there is a loop and the link was
         * formed by the sending remote we mute the link.
         */
        if (DPS_MeshHasLoop(node, remote, meshId)) {
            DPS_BitVectorFree(interests);
            DPS_BitVectorFree(needs);
            ret = DPS_MuteRemoteNode(node, remote);
        } else {
            remote->inbound.meshId = *meshId;
            ret = UpdateInboundInterests(node, remote, interests, needs, !syncReceived);
        }
    }
    /*
     * Check if application waiting for a completion callback. Even if there was
     * an error we are ok to complete because all we need is a response.
     */
    if (remote->completion) {
        DPS_RemoteCompletion(node, remote, DPS_OK);
    }
    DPS_UnlockNode(node);
    /*
     * Schedule background tasks
     */
    if (ret == DPS_OK) {
        DPS_UpdatePubs(node, NULL);
        DPS_UpdateSubs(node, NULL);
    }
    return ret;
}

DPS_Status DPS_Subscribe(DPS_Subscription* sub, DPS_PublicationHandler handler)
{
    size_t i;
    DPS_Status ret = DPS_OK;
    DPS_Node* node = sub ? sub->node : NULL;

    if (!node) {
        return DPS_ERR_NULL;
    }
    if (!node->loop) {
        return DPS_ERR_NOT_STARTED;
    }
    sub->handler = handler;
    sub->bf = DPS_BitVectorAlloc();
    sub->needs = DPS_BitVectorAllocFH();
    if (!sub->bf || !sub->needs) {
        return DPS_ERR_RESOURCES;
    }
    /*
     * Add the topics to the bloom filter
     */
    for (i = 0; i < sub->numTopics; ++i) {
        ret = DPS_AddTopic(sub->bf, sub->topics[i], node->separators, DPS_SubTopic);
        if (ret != DPS_OK) {
            break;
        }
    }
    if (ret != DPS_OK) {
        return ret;
    }

    DPS_DBGPRINT("Subscribing to %zu topics\n", sub->numTopics);
    DPS_DumpTopics((const char**)sub->topics, sub->numTopics);

    DPS_BitVectorFuzzyHash(sub->needs, sub->bf);
    /*
     * Protect the node while we update it
     */
    DPS_LockNode(node);
    sub->next = node->subscriptions;
    node->subscriptions = sub;
    ret = DPS_CountVectorAdd(node->interests, sub->bf);
    if (ret == DPS_OK) {
        ret = DPS_CountVectorAdd(node->needs, sub->needs);
    }
    DPS_UnlockNode(node);
    if (ret == DPS_OK) {
        DPS_UpdateSubs(node, NULL);
    }
    return ret;
}

DPS_Status DPS_SetSubscriptionData(DPS_Subscription* sub, void* data)
{
    if (sub) {
        sub->userData = data;
        return DPS_OK;
    } else {
        return DPS_ERR_NULL;
    }
}

void* DPS_GetSubscriptionData(DPS_Subscription* sub)
{
    return sub ? sub->userData : NULL;
}

void DPS_DumpSubscriptions(DPS_Node* node)
{
    DPS_Subscription* sub;

    DPS_DBGPRINT("Current subscriptions:\n");
    for (sub = node->subscriptions; sub != NULL; sub = sub->next) {
        DPS_DumpTopics((const char**)sub->topics, sub->numTopics);
    }
}
