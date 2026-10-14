// The kernel exports reNut replaces (see kernel_overrides.cpp). Each one is
// defined there as renut_imp_<name>; the build renames the game's calls to it.
X(VdSwap)
X(XAudioGetVoiceCategoryVolumeChangeMask)
X(XAudioRegisterRenderDriverClient)
X(XAudioUnregisterRenderDriverClient)
X(XAudioSubmitRenderDriverFrame)
// The player's pause parks a guest thread at its next wait, delay or yield.
X(KeDelayExecutionThread)
X(NtYieldExecution)
X(KeWaitForSingleObject)
X(NtWaitForSingleObjectEx)
X(KeWaitForMultipleObjects)
X(NtWaitForMultipleObjectsEx)
X(NtSignalAndWaitForSingleObjectEx)
// The game's text entry (the Xbox keyboard) goes to the app's text box: text_input_bridge.cpp.
X(XamShowKeyboardUI)
// A static KEVENT the title initialises again while a thread waits on it.
X(KeInitializeEvent)
