#include "WKWebContext.h"

#include "Logging.h"
#include "WKWebView.h"
#include "WKWebsiteDataManager.h"

#include <unistd.h>

/***********************************************************************************************************************
 * JNI mapping with Java WKWebContext class
 **********************************************************************************************************************/

class JNIWKWebContextCache;
const JNIWKWebContextCache& getJNIWKWebContextCache();

class JNIWKWebContextCache final : public JNI::TypedClass<JNIWKWebContext> {
public:
    JNIWKWebContextCache();

    static WebKitWebView* onCreateWebViewForAutomation(WKWebContext* wkWebContext, WebKitWebContext* /*webContext*/)
    {
        WebKitWebView* view = nullptr;
        try {
            const jlong wkWebViewPtr = getJNIWKWebContextCache().m_createPageForAutomation.invoke(
                wkWebContext->m_webContextJavaInstance.get());
            if (wkWebViewPtr > 0) {
                auto* wkWebView = reinterpret_cast<WKWebView*>(wkWebViewPtr); // NOLINT(performance-no-int-to-ptr)
                view = wkWebView->webView();
            }
        } catch (const std::exception& ex) {
            Logging::logError("Cannot send web view event to Java runtime (%s)", ex.what());
        }
        return view;
    }

    static void onAutomationStarted(
        WKWebContext* wkWebContext, WebKitAutomationSession* session, WebKitWebContext* /*webContext*/) noexcept
    {
        auto* info = webkit_application_info_new();
        webkit_application_info_set_name(info, "MiniBrowser");
        webkit_application_info_set_version(info, WEBKIT_MAJOR_VERSION, WEBKIT_MINOR_VERSION, WEBKIT_MICRO_VERSION);
        webkit_automation_session_set_application_info(session, info);
        webkit_application_info_unref(info);

        g_signal_connect_swapped(
            session, "create-web-view", G_CALLBACK(JNIWKWebContextCache::onCreateWebViewForAutomation), wkWebContext);
    }

private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const JNI::Method<jlong()> m_createPageForAutomation;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

    static jlong nativeInit(JNIEnv* env, jobject obj, jboolean automationMode);
    static void nativeDestroy(JNIEnv* env, jobject obj, jlong wkWebContextPtr) noexcept;
};

const JNIWKWebContextCache& getJNIWKWebContextCache()
{
    static const JNIWKWebContextCache s_singleton;
    return s_singleton;
}

JNIWKWebContextCache::JNIWKWebContextCache()
    : JNI::TypedClass<JNIWKWebContext>(true)
    , m_createPageForAutomation(getMethod<jlong()>("createWKWebViewForAutomation"))
{
    registerNativeMethods(JNI::NativeMethod<jlong(jboolean)>("nativeInit", JNIWKWebContextCache::nativeInit),
        JNI::NativeMethod<void(jlong)>("nativeDestroy", JNIWKWebContextCache::nativeDestroy));
}

jlong JNIWKWebContextCache::nativeInit(JNIEnv* env, jobject obj, jboolean automationMode)
{
    Logging::logDebug("WKWebContext::nativeInit(%p, %d, %d) [tid %d]", obj, gettid());
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    auto* wkWebContext = new WKWebContext(
        env, reinterpret_cast<JNIWKWebContext>(obj), static_cast<unsigned int>(automationMode) != 0U);
    return reinterpret_cast<jlong>(wkWebContext);
}

void JNIWKWebContextCache::nativeDestroy(JNIEnv* /*env*/, jobject /*obj*/, jlong wkWebContextPtr) noexcept
{
    Logging::logDebug("WKWebContext::nativeDestroy() [tid %d]", gettid());
    auto* wkWebContext = reinterpret_cast<WKWebContext*>(wkWebContextPtr); // NOLINT(performance-no-int-to-ptr)
    delete wkWebContext; // NOLINT(cppcoreguidelines-owning-memory)
}

/***********************************************************************************************************************
 * Native WKWebContext class implementation
 **********************************************************************************************************************/

void WKWebContext::configureJNIMappings() { getJNIWKWebContextCache(); }

WKWebContext::WKWebContext(JNIEnv* env, JNIWKWebContext jniWKWebContext, bool automationMode)
    : m_webContextJavaInstance(JNI::createTypedProtectedRef(env, jniWKWebContext, true))
{
    m_automationMode = automationMode;

    m_webContext = {webkit_web_context_new(), [](auto* ptr) { g_object_unref(ptr); }};
    webkit_web_context_set_automation_allowed(m_webContext.get(), automationMode ? TRUE : FALSE);
    if (automationMode) {
        g_signal_connect_swapped(
            m_webContext.get(), "automation-started", G_CALLBACK(JNIWKWebContextCache::onAutomationStarted), this);
    }
}
