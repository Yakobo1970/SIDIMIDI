#include "UpdateCheck.h"

namespace sm
{
namespace
{
constexpr juce::int64 kDayMs = 24LL * 60 * 60 * 1000;

juce::PropertiesFile::Options settingsOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "update";
    o.folderName = "SIDIMIDI";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    o.millisecondsBeforeSaving = -1;   // saved explicitly (also from the background thread)
    return o;
}
} // namespace

UpdateCheck::UpdateCheck() : juce::Thread ("SIDiMIDI update check")
{
    props = std::make_unique<juce::PropertiesFile> (settingsOptions());
    latest = props->getValue ("updateLatest");
    // test hook (UiShot): pretend this release exists, no network
    testVersion = juce::SystemStats::getEnvironmentVariable ("SIDIMIDI_UPDATE_TEST", {});
    if (testVersion.isNotEmpty()) { latest = testVersion; return; }
    startIfDue (false);
}

UpdateCheck::~UpdateCheck()
{
    stopThread (6000);
}

bool UpdateCheck::enabled() const
{
    const juce::ScopedLock sl (lock);
    return props->getBoolValue ("updateCheck", true);
}

void UpdateCheck::setEnabled (bool on)
{
    {
        const juce::ScopedLock sl (lock);
        props->setValue ("updateCheck", on);
        props->saveIfNeeded();
    }
    if (on) startIfDue (true);
}

juce::String UpdateCheck::newerVersion() const
{
    const juce::ScopedLock sl (lock);
    if (dismissed || ! props->getBoolValue ("updateCheck", true)) return {};
    return isNewer (latest, SM_VERSION_STRING) ? latest : juce::String();
}

void UpdateCheck::dismiss()
{
    const juce::ScopedLock sl (lock);
    dismissed = true;
}

void UpdateCheck::startIfDue (bool force)
{
    if (testVersion.isNotEmpty() || isThreadRunning() || ! enabled()) return;
    const auto last = props->getValue ("updateLast", "0").getLargeIntValue();
    const auto now = juce::Time::currentTimeMillis();
    if (! force && now - last < kDayMs && now >= last) return;
    startThread (juce::Thread::Priority::low);
}

void UpdateCheck::run()
{
    const auto tag = fetchLatest (5000, [this] { return ! threadShouldExit(); });
    if (threadShouldExit()) return;
    const juce::ScopedLock sl (lock);
    props->setValue ("updateLast", juce::String (juce::Time::currentTimeMillis()));   // failures too: next try tomorrow / next switch-on
    if (tag.isNotEmpty())
    {
        latest = tag;
        props->setValue ("updateLatest", tag);
    }
    props->saveIfNeeded();
}

juce::String UpdateCheck::fetchLatest (int timeoutMs, std::function<bool()> keepGoing)
{
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs (timeoutMs)
                    .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: SIDiMIDI/" SM_VERSION_STRING);
    auto stream = juce::URL (kApiUrl).createInputStream (std::move (opts).withProgressCallback ([keepGoing] (int, int) { return keepGoing == nullptr || keepGoing(); }));
    if (stream == nullptr) return {};
    const auto json = juce::JSON::parse (stream->readEntireStreamAsString());
    if (json.getProperty ("draft", false) || json.getProperty ("prerelease", false)) return {};
    auto tag = json.getProperty ("tag_name", {}).toString().trim();
    if (tag.startsWithIgnoreCase ("v")) tag = tag.substring (1);
    return tag.containsOnly ("0123456789.") && tag.containsAnyOf ("0123456789") ? tag : juce::String();
}

bool UpdateCheck::isNewer (const juce::String& candidate, const juce::String& current)
{
    auto parts = [] (juce::String s) {
        if (s.startsWithIgnoreCase ("v")) s = s.substring (1);
        juce::StringArray a;
        a.addTokens (s.trim(), ".", {});
        return a;
    };
    const auto a = parts (candidate), b = parts (current);
    if (a.isEmpty() || candidate.trim().isEmpty()) return false;
    for (int i = 0; i < juce::jmax (a.size(), b.size()); ++i)
    {
        const int x = a[i].getIntValue(), y = b[i].getIntValue();
        if (x != y) return x > y;
    }
    return false;
}
} // namespace sm
