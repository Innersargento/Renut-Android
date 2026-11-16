// The kernel exports reNut replaces (see kernel_overrides.cpp). Each one is
// defined there as renut_imp_<name>; the build renames the game's calls to it.
// The SDK's XAM keeps a reference to the title workspace from before the kernel recreates it at the size the XEX declares.
X(XamAlloc)
X(XamAllocEx)
X(XamFree)
// The console does not return from unregistering an audio render client while its callback is running.
X(XAudioRegisterRenderDriverClient)
X(XAudioUnregisterRenderDriverClient)
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
// Local multiplayer (xam_users.cpp): a second gamepad is a second signed-in guest user.
X(XamUserGetXUID)
X(XamUserGetSigninState)
X(XamUserGetSigninInfo)
X(XamUserGetName)
X(XamUserGetGamerTag)
X(XamUserReadProfileSettings)
X(XamUserReadProfileSettingsEx)
X(XamUserWriteProfileSettings)
X(XamUserCheckPrivilege)
X(XamUserContentRestrictionGetFlags)
X(XamUserContentRestrictionGetRating)
X(XamUserGetMembershipTier)
X(XamUserAreUsersFriends)
X(XamShowSigninUI)
X(XamInputGetState)
X(XamInputGetKeystroke)
X(XamInputGetKeystrokeEx)
X(XamUserGetDeviceContext)
