#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "ea_EASPHandler.h"

/*
 * EASP's log switch. The engine calls it once during initJNI and never reads
 * anything back, so accepting the flag and dropping it is the whole contract;
 * turning it on would only add EA's server chatter to a log that has none of
 * its servers to talk to.
 */
void EASPHandler::setLogEnabled(JNIEnv *env, jobject obj, jboolean enabled)
{
    (void)env;
    (void)obj;
    trace("EASPHandler.setLogEnabled(%d) ignored", (int)enabled);
}

const ManagedMethod EASPHandlerMethods[] = {
    ManagedMethod::Register<&EASPHandler::setLogEnabled>(
        EASPHandler::clazz, "setLogEnabled", "(Z)V"),
    {NULL},
};

Class EASPHandler::clazz = {
    .classpath        = "com/ea/easp/EASPHandler",
    .classname        = "EASPHandler",
    .managed_methods  = EASPHandlerMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(EASPHandler),
};

static const int registered = ClassRegistry::register_class(EASPHandler::clazz);
