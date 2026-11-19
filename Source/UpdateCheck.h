#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>

namespace sm
{
// Looks up the newest GitHub release at most once a day, in the background.
// Sends nothing but the request itself; can be switched off. Shared by all editors of the process
// (use juce::SharedResourcePointer<UpdateCheck>).
class UpdateCheck : private juce::Thread
{
public:
    static constexpr const char* kApiUrl = "https://api.github.com/repos/Yakobo1970/SIDIMIDI/releases/latest";
    static constexpr const char* kReleasesUrl = "https://github.com/Yakobo1970/SIDIMIDI/releases/latest";

    UpdateCheck();
    ~UpdateCheck() override;

    bool enabled() const;
    void setEnabled (bool on);            // switching on looks up right away
    juce::String newerVersion() const;    // the newer release ("1.0.1"), empty if none / switched off
    void dismiss();                       // hide the hint until the next start
    bool checking() const { return isThreadRunning(); }

    // "v1.2.10" > "1.2.9"; non-numeric parts count as 0
    static bool isNewer (const juce::String& candidate, const juce::String& current);
    // blocking request (ArchivTest); returns the tag without "v", empty on failure
    static juce::String fetchLatest (int timeoutMs, std::function<bool()> keepGoing = {});

private:
    void run() override;
    void startIfDue (bool force);
    std::unique_ptr<juce::PropertiesFile> props;
    juce::String latest, testVersion;
    bool dismissed = false;
    mutable juce::CriticalSection lock;
};
} // namespace sm
