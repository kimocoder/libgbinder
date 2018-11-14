/*
 * Copyright (C) 2018 Jolla Ltd.
 * Copyright (C) 2018 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "gbinder_servicemanager_p.h"
#include "gbinder_client_p.h"
#include "gbinder_local_object_p.h"
#include "gbinder_remote_object_p.h"
#include "gbinder_driver.h"
#include "gbinder_ipc.h"
#include "gbinder_log.h"

#include <gbinder_client.h>

#include <gutil_idlepool.h>

#include <errno.h>

typedef struct gbinder_servicemanager_watch {
    char* name;
    char* detail;
    GQuark quark;
    gboolean watched;
} GBinderServiceManagerWatch;

struct gbinder_servicemanager_priv {
    GHashTable* watch_table;
};

G_DEFINE_ABSTRACT_TYPE(GBinderServiceManager, gbinder_servicemanager,
    G_TYPE_OBJECT)

#define PARENT_CLASS gbinder_servicemanager_parent_class
#define GBINDER_SERVICEMANAGER(obj) \
    G_TYPE_CHECK_INSTANCE_CAST((obj), GBINDER_TYPE_SERVICEMANAGER, \
    GBinderServiceManager)
#define GBINDER_SERVICEMANAGER_CLASS(klass) \
    G_TYPE_CHECK_CLASS_CAST((klass), GBINDER_TYPE_SERVICEMANAGER, \
    GBinderServiceManagerClass)
#define GBINDER_SERVICEMANAGER_GET_CLASS(obj) \
    G_TYPE_INSTANCE_GET_CLASS((obj), GBINDER_TYPE_SERVICEMANAGER, \
    GBinderServiceManagerClass)
#define GBINDER_IS_SERVICEMANAGER_TYPE(klass) \
    G_TYPE_CHECK_CLASS_TYPE(klass, GBINDER_TYPE_SERVICEMANAGER)

enum gbinder_servicemanager_signal {
    SIGNAL_REGISTRATION,
    SIGNAL_COUNT
};

static const char SIGNAL_REGISTRATION_NAME[] = "servicemanager-registration";
#define DETAIL_LEN 32

static guint gbinder_servicemanager_signals[SIGNAL_COUNT] = { 0 };

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
GBinderServiceManagerClass*
gbinder_servicemanager_class_ref(
    GType type)
{
    if (G_LIKELY(type)) {
        GTypeClass* klass = g_type_class_ref(type);
        if (klass) {
            if (GBINDER_IS_SERVICEMANAGER_TYPE(klass)) {
                return GBINDER_SERVICEMANAGER_CLASS(klass);
            }
            g_type_class_unref(klass);
        }
    }
    return NULL;
}

static
GBinderServiceManagerWatch*
gbinder_servicemanager_watch_new(
    const char* name)
{
    GBinderServiceManagerWatch* watch = g_new0(GBinderServiceManagerWatch, 1);

    watch->name = g_strdup(name);
    watch->detail = g_compute_checksum_for_string(G_CHECKSUM_MD5, name, -1);
    watch->quark = g_quark_from_string(watch->detail);
    return watch;
}

static
void
gbinder_servicemanager_watch_free(
    gpointer data)
{
    GBinderServiceManagerWatch* watch = data;

    g_free(watch->name);
    g_free(watch->detail);
    g_free(watch);
}

typedef struct gbinder_servicemanager_list_tx_data {
    GBinderServiceManager* sm;
    GBinderServiceManagerListFunc func;
    char** result;
    void* user_data;
} GBinderServiceManagerListTxData;

static
void
gbinder_servicemanager_list_tx_exec(
    const GBinderIpcTx* tx)
{
    GBinderServiceManagerListTxData* data = tx->user_data;

    data->result = GBINDER_SERVICEMANAGER_GET_CLASS(data->sm)->list(data->sm);
}

static
void
gbinder_servicemanager_list_tx_done(
    const GBinderIpcTx* tx)
{
    GBinderServiceManagerListTxData* data = tx->user_data;

    if (!data->func(data->sm, data->result, data->user_data)) {
        g_strfreev(data->result);
    }
}

static
void
gbinder_servicemanager_list_tx_free(
    gpointer user_data)
{
    GBinderServiceManagerListTxData* data = user_data;

    gbinder_servicemanager_unref(data->sm);
    g_slice_free(GBinderServiceManagerListTxData, data);
}

typedef struct gbinder_servicemanager_get_service_tx {
    GBinderServiceManager* sm;
    GBinderServiceManagerGetServiceFunc func;
    GBinderRemoteObject* obj;
    int status;
    char* name;
    void* user_data;
} GBinderServiceManagerGetServiceTxData;

static
void
gbinder_servicemanager_get_service_tx_exec(
    const GBinderIpcTx* tx)
{
    GBinderServiceManagerGetServiceTxData* data = tx->user_data;

    data->obj = GBINDER_SERVICEMANAGER_GET_CLASS(data->sm)->get_service
            (data->sm, data->name, &data->status);
}

static
void
gbinder_servicemanager_get_service_tx_done(
    const GBinderIpcTx* tx)
{
    GBinderServiceManagerGetServiceTxData* data = tx->user_data;

    data->func(data->sm, data->obj, data->status, data->user_data);
}

static
void
gbinder_servicemanager_get_service_tx_free(
    gpointer user_data)
{
    GBinderServiceManagerGetServiceTxData* data = user_data;

    gbinder_servicemanager_unref(data->sm);
    gbinder_remote_object_unref(data->obj);
    g_free(data->name);
    g_slice_free(GBinderServiceManagerGetServiceTxData, data);
}

typedef struct gbinder_servicemanager_add_service_tx {
    GBinderServiceManager* sm;
    GBinderServiceManagerAddServiceFunc func;
    GBinderLocalObject* obj;
    int status;
    char* name;
    void* user_data;
} GBinderServiceManagerAddServiceTxData;

static
void
gbinder_servicemanager_add_service_tx_exec(
    const GBinderIpcTx* tx)
{
    GBinderServiceManagerAddServiceTxData* data = tx->user_data;

    data->status = GBINDER_SERVICEMANAGER_GET_CLASS(data->sm)->add_service
            (data->sm, data->name, data->obj);
}

static
void
gbinder_servicemanager_add_service_tx_done(
    const GBinderIpcTx* tx)
{
    GBinderServiceManagerAddServiceTxData* data = tx->user_data;

    data->func(data->sm, data->status, data->user_data);
}

static
void
gbinder_servicemanager_add_service_tx_free(
    gpointer user_data)
{
    GBinderServiceManagerAddServiceTxData* data = user_data;

    gbinder_servicemanager_unref(data->sm);
    gbinder_local_object_unref(data->obj);
    g_free(data->name);
    g_slice_free(GBinderServiceManagerAddServiceTxData, data);
}

/*==========================================================================*
 * Internal interface
 *==========================================================================*/

GBinderServiceManager*
gbinder_servicemanager_new_with_type(
    GType type,
    const char* dev)
{
    GBinderServiceManager* self = NULL;
    GBinderServiceManagerClass* klass = gbinder_servicemanager_class_ref(type);

    if (klass) {
        GBinderIpc* ipc;

        if (!dev) dev = klass->default_device;
        ipc = gbinder_ipc_new(dev, klass->rpc_protocol);
        if (ipc) {
            GBinderRemoteObject* object = gbinder_ipc_get_remote_object
                (ipc, klass->handle);

            if (object) {
                /* Lock */
                g_mutex_lock(&klass->mutex);
                if (klass->table) {
                    self = g_hash_table_lookup(klass->table, dev);
                }
                if (self) {
                    gbinder_servicemanager_ref(self);
                } else {
                    char* key = g_strdup(dev); /* Owned by the hashtable */

                    GVERBOSE_("%s", dev);
                    self = g_object_new(type, NULL);
                    self->client = gbinder_client_new(object, klass->iface);
                    self->dev = gbinder_remote_object_dev(object);
                    if (!klass->table) {
                        klass->table = g_hash_table_new_full(g_str_hash,
                            g_str_equal, g_free, NULL);
                    }
                    g_hash_table_replace(klass->table, key, self);
                }
                g_mutex_unlock(&klass->mutex);
                /* Unlock */
                gbinder_remote_object_unref(object);
            }
            gbinder_ipc_unref(ipc);
        }
        g_type_class_unref(klass);
    }
    return self;
}

void
gbinder_servicemanager_service_registered(
    GBinderServiceManager* self,
    const char* name)
{
    GBinderServiceManagerClass* klass = GBINDER_SERVICEMANAGER_GET_CLASS(self);
    GBinderServiceManagerPriv* priv = self->priv;
    GBinderServiceManagerWatch* watch = NULL;
    const char* normalized_name;
    char* tmp_name = NULL;

    switch (klass->check_name(self, name)) {
    case GBINDER_SERVICEMANAGER_NAME_OK:
        normalized_name = name;
        break;
    case GBINDER_SERVICEMANAGER_NAME_NORMALIZE:
        normalized_name = tmp_name = klass->normalize_name(self, name);
        break;
    default:
        normalized_name = NULL;
        break;
    }
    if (normalized_name) {
        watch = g_hash_table_lookup(priv->watch_table, normalized_name);
    }
    g_free(tmp_name);
    g_signal_emit(self, gbinder_servicemanager_signals[SIGNAL_REGISTRATION],
        watch ? watch->quark : 0, name);
}

/*==========================================================================*
 * Interface
 *==========================================================================*/

GBinderServiceManager*
gbinder_servicemanager_new(
    const char* dev)
{
    if (!g_strcmp0(dev, GBINDER_DEFAULT_HWBINDER)) {
        return gbinder_hwservicemanager_new(dev);
    } else {
        return gbinder_defaultservicemanager_new(dev);
    }
}

GBinderLocalObject*
gbinder_servicemanager_new_local_object(
    GBinderServiceManager* self,
    const char* iface,
    GBinderLocalTransactFunc txproc,
    void* user_data)
{
    if (G_LIKELY(self)) {
        return gbinder_ipc_new_local_object(gbinder_client_ipc(self->client),
            iface, txproc, user_data);
    }
    return NULL;
}

GBinderServiceManager*
gbinder_servicemanager_ref(
    GBinderServiceManager* self)
{
    if (G_LIKELY(self)) {
        g_object_ref(GBINDER_SERVICEMANAGER(self));
    }
    return self;
}

void
gbinder_servicemanager_unref(
    GBinderServiceManager* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(GBINDER_SERVICEMANAGER(self));
    }
}

gulong
gbinder_servicemanager_list(
    GBinderServiceManager* self,
    GBinderServiceManagerListFunc func,
    void* user_data)
{
    if (G_LIKELY(self) && func) {
        GBinderServiceManagerListTxData* data =
            g_slice_new0(GBinderServiceManagerListTxData);

        data->sm = gbinder_servicemanager_ref(self);
        data->func = func;
        data->user_data = user_data;

        return gbinder_ipc_transact_custom(gbinder_client_ipc(self->client),
            gbinder_servicemanager_list_tx_exec,
            gbinder_servicemanager_list_tx_done,
            gbinder_servicemanager_list_tx_free, data);
    }
    return 0;
}

char**
gbinder_servicemanager_list_sync(
    GBinderServiceManager* self)
{
    if (G_LIKELY(self)) {
        return GBINDER_SERVICEMANAGER_GET_CLASS(self)->list(self);
    }
    return NULL;
}

gulong
gbinder_servicemanager_get_service(
    GBinderServiceManager* self,
    const char* name,
    GBinderServiceManagerGetServiceFunc func,
    void* user_data)
{
    if (G_LIKELY(self) && func && name) {
        GBinderServiceManagerGetServiceTxData* data =
            g_slice_new0(GBinderServiceManagerGetServiceTxData);

        data->sm = gbinder_servicemanager_ref(self);
        data->func = func;
        data->name = g_strdup(name);
        data->user_data = user_data;
        data->status = (-EFAULT);

        return gbinder_ipc_transact_custom(gbinder_client_ipc(self->client),
            gbinder_servicemanager_get_service_tx_exec,
            gbinder_servicemanager_get_service_tx_done,
            gbinder_servicemanager_get_service_tx_free, data);
    }
    return 0;
}

GBinderRemoteObject* /* autoreleased */
gbinder_servicemanager_get_service_sync(
    GBinderServiceManager* self,
    const char* name,
    int* status)
{
    GBinderRemoteObject* obj = NULL;

    if (G_LIKELY(self) && name) {
        obj = GBINDER_SERVICEMANAGER_GET_CLASS(self)->get_service
            (self, name, status);
        if (!self->pool) {
            self->pool = gutil_idle_pool_new();
        }
        gutil_idle_pool_add_object(self->pool, obj);
    } else if (status) {
        *status = (-EINVAL);
    }
    return obj;
}

gulong
gbinder_servicemanager_add_service(
    GBinderServiceManager* self,
    const char* name,
    GBinderLocalObject* obj,
    GBinderServiceManagerAddServiceFunc func,
    void* user_data)
{
    if (G_LIKELY(self) && func && name) {
        GBinderServiceManagerAddServiceTxData* data =
            g_slice_new0(GBinderServiceManagerAddServiceTxData);

        data->sm = gbinder_servicemanager_ref(self);
        data->obj = gbinder_local_object_ref(obj);
        data->func = func;
        data->name = g_strdup(name);
        data->user_data = user_data;
        data->status = (-EFAULT);

        return gbinder_ipc_transact_custom(gbinder_client_ipc(self->client),
            gbinder_servicemanager_add_service_tx_exec,
            gbinder_servicemanager_add_service_tx_done,
            gbinder_servicemanager_add_service_tx_free, data);
    }
    return 0;
}

int
gbinder_servicemanager_add_service_sync(
    GBinderServiceManager* self,
    const char* name,
    GBinderLocalObject* obj)
{
    if (G_LIKELY(self) && name && obj) {
        return GBINDER_SERVICEMANAGER_GET_CLASS(self)->add_service
            (self, name, obj);
    } else {
        return (-EINVAL);
    }
}

void
gbinder_servicemanager_cancel(
    GBinderServiceManager* self,
    gulong id)
{
    if (G_LIKELY(self)) {
        gbinder_ipc_cancel(gbinder_client_ipc(self->client), id);
    }
}

gulong
gbinder_servicemanager_add_registration_handler(
    GBinderServiceManager* self,
    const char* name,
    GBinderServiceManagerRegistrationFunc func,
    void* data) /* Since 1.0.13 */
{
    gulong id = 0;

    if (G_LIKELY(self) && G_LIKELY(func)) {
        char* tmp_name = NULL;
        GBinderServiceManagerClass* klass =
            GBINDER_SERVICEMANAGER_GET_CLASS(self);

        switch (klass->check_name(self, name)) {
        case GBINDER_SERVICEMANAGER_NAME_OK:
            break;
        case GBINDER_SERVICEMANAGER_NAME_NORMALIZE:
            name = tmp_name = klass->normalize_name(self, name);
            break;
        default:
            name = NULL;
            break;
        }
        if (name) {
            GBinderServiceManagerPriv* priv = self->priv;
            GBinderServiceManagerWatch* watch = NULL;

            watch = g_hash_table_lookup(priv->watch_table, name);
            if (!watch) {
                watch = gbinder_servicemanager_watch_new(name);
                g_hash_table_insert(priv->watch_table, watch->name, watch);
            }
            if (!watch->watched) {
                watch->watched = klass->watch(self, name);
                if (watch->watched) {
                    GDEBUG("Watching %s", watch->name);
                } else {
                    GWARN("Failed to watch %s", watch->name);
                }
            }

            id = g_signal_connect_closure_by_id(self,
                gbinder_servicemanager_signals[SIGNAL_REGISTRATION],
                watch->quark, g_cclosure_new(G_CALLBACK(func), data, NULL),
                FALSE);
        }
        g_free(tmp_name);
    }
    return id;
}

void
gbinder_servicemanager_remove_handler(
    GBinderServiceManager* self,
    gulong id) /* Since 1.0.13 */
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        GBinderServiceManagerClass* klass =
            GBINDER_SERVICEMANAGER_GET_CLASS(self);
        GBinderServiceManagerPriv* priv = self->priv;
        GHashTableIter it;
        gpointer value;

        g_signal_handler_disconnect(self, id);
        g_hash_table_iter_init(&it, priv->watch_table);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            GBinderServiceManagerWatch* watch = value;

            if (watch->watched && !g_signal_has_handler_pending(self,
                gbinder_servicemanager_signals[SIGNAL_REGISTRATION],
                watch->quark, TRUE)) {
                /* This must be the one we have just removed */
                GDEBUG("Unwatching %s", watch->name);
                watch->watched = FALSE;
                klass->unwatch(self, watch->name);
                break;
            }
        }
    }
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
gbinder_servicemanager_init(
    GBinderServiceManager* self)
{
    GBinderServiceManagerPriv* priv = G_TYPE_INSTANCE_GET_PRIVATE(self,
        GBINDER_TYPE_SERVICEMANAGER, GBinderServiceManagerPriv);

    self->priv = priv;
    priv->watch_table = g_hash_table_new_full(g_str_hash, g_str_equal,
        NULL, gbinder_servicemanager_watch_free);
}

static
void
gbinder_servicemanager_dispose(
    GObject* object)
{
    GBinderServiceManager* self = GBINDER_SERVICEMANAGER(object);
    GBinderServiceManagerClass* klass = GBINDER_SERVICEMANAGER_GET_CLASS(self);

    GVERBOSE_("%s", self->dev);
    /* Lock */
    g_mutex_lock(&klass->mutex);

    /*
     * The follow can happen:
     *
     * 1. Last reference goes away.
     * 2. gbinder_servicemanager_dispose() is invoked by glib
     * 3. Before gbinder_servicemanager_dispose() grabs the
     *    lock, gbinder_servicemanager_new() gets there first,
     *    finds the object in the hashtable, bumps its refcount
     *    (under the lock) and returns the reference to the caller.
     * 4. gbinder_servicemanager_dispose() gets its lock, finds
     *    that the object's refcount is greater than zero and leaves
     *    the object in the table.
     *
     * It's OK for a GObject to get re-referenced in dispose.
     * glib will recheck the refcount once dispose returns,
     * gbinder_servicemanager_finalize() will not be called
     * this time around.
     */
    if (klass->table && g_atomic_int_get(&object->ref_count) <= 1) {
        g_hash_table_remove(klass->table, self->dev);
        if (g_hash_table_size(klass->table) == 0) {
            g_hash_table_unref(klass->table);
            klass->table = NULL;
        }
    }
    g_mutex_unlock(&klass->mutex);
    /* Unlock */
    G_OBJECT_CLASS(PARENT_CLASS)->dispose(object);
}

static
void
gbinder_servicemanager_finalize(
    GObject* object)
{
    GBinderServiceManager* self = GBINDER_SERVICEMANAGER(object);
    GBinderServiceManagerPriv* priv = self->priv;

    g_hash_table_destroy(priv->watch_table);
    gutil_idle_pool_destroy(self->pool);
    gbinder_client_unref(self->client);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
gbinder_servicemanager_class_init(
    GBinderServiceManagerClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);

    g_mutex_init(&klass->mutex);
    g_type_class_add_private(klass, sizeof(GBinderServiceManagerPriv));
    object_class->dispose = gbinder_servicemanager_dispose;
    object_class->finalize = gbinder_servicemanager_finalize;
    gbinder_servicemanager_signals[SIGNAL_REGISTRATION] =
        g_signal_new(SIGNAL_REGISTRATION_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST | G_SIGNAL_DETAILED, 0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_STRING);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
