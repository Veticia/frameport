// Per-game OpenXR identity. 0/2 preserve the runtime's original Quest 2 compatibility behaviour.
// OVRPort owns XR_META_headset_id above this layer; the APK patch rewrites its UUID.
static int headset_model;

static void headset_model_setting(const char *line) {
    int value;
    char trailing;
    if (sscanf(line, "headset_model=%d %c", &value, &trailing) == 1 && value >= 2 && value <= 5)
        headset_model = value;
}

static void headset_model_properties(XrSystemProperties *properties) {
    const char *name = NULL;
    switch (headset_model) {
        case 3: name = "Meta Quest 3"; break;
        case 4: name = "Meta Quest 3S"; break;
        case 5: name = "Meta Quest Pro"; break;
    }
    if (properties && name)
        snprintf(properties->systemName, sizeof(properties->systemName), "%s", name);
}
