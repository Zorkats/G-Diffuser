// Preview and queue admission must share validation; only the disk thread may install the staged bytes.

static void gdx_import_check_disk_state(GdxContentImportEntry* entry, uint32_t* errors, uint32_t* warnings) {
    int32_t payloadSize = gdx_import_expected_payload_size(entry->contentType);
    int32_t freeSize;
    int32_t probe;
    int count;
    int warnThreshold;
    const char* classPrefix;
    char twinExt[6];

    if (gdx_content_mfs_busy() || gdx_recovery_blocks_writes()) {
        *errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
        entry->classFileCount = -1;
        return;
    }

    classPrefix = (entry->contentType == GDX_CONTENT_TYPE_TRACK) ? "CRS" : "CAR";
    count = gdx_content_count_extension_class(classPrefix);
    if (count < 0) {
        *errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
        entry->classFileCount = -1;
        return;
    }
    entry->classFileCount = count;

    probe = gdx_content_file_exists(entry->name, entry->extension);
    if (probe < 0) {
        *errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
        return;
    }
    if (probe != 0) {
        *warnings |= GDX_IMPORT_WARN_OVERWRITE;
    }

    if (entry->contentType == GDX_CONTENT_TYPE_TRACK) {
        /* The install deletes the opposite-extension twin, mirroring the editor's save op
         * (func_xk2_800EB938, course_edit/19C470.c:478-486). */
        memcpy(twinExt, entry->extension[3] == 'D' ? "CRSE" : "CRSD", 5);
        twinExt[5] = '\0';
        probe = gdx_content_file_exists(entry->name, twinExt);
        if (probe > 0) {
            *warnings |= GDX_IMPORT_WARN_TWIN_DELETE;
        }
        if (entry->extension[3] == 'E') {
            /* CRSE tracks cannot sit in the edit cup; the import clears a slot naming it, so say
             * so up front. */
            int i;
            for (i = 0; i < 6; i++) {
                char slot[GDX_EDIT_CUP_NAME_SIZE + 1];
                memcpy(slot, gEditCupTrackNames[i], GDX_EDIT_CUP_NAME_SIZE);
                slot[GDX_EDIT_CUP_NAME_SIZE] = '\0';
                if (slot[0] != '\0' && strcmp(slot, entry->name) == 0) {
                    *warnings |= GDX_IMPORT_WARN_CUP_CLEAR;
                    break;
                }
            }
        }
    }

    /* An overwrite replaces a file and does not grow the class, so the cap only gates new
     * names. */
    if ((*warnings & GDX_IMPORT_WARN_OVERWRITE) == 0) {
        if (count >= 100) {
            *errors |= GDX_IMPORT_ERR_QUOTA_FULL;
        } else {
            warnThreshold = CVarGetInteger("gEnhancements.Content.QuotaWarnThreshold", 90);
            if (warnThreshold > 0 && count >= warnThreshold) {
                *warnings |= GDX_IMPORT_WARN_QUOTA;
            }
        }
    }

    freeSize = Mfs_RamGetFreeSize();
    if (freeSize >= 0 && freeSize < payloadSize) {
        *errors |= GDX_IMPORT_ERR_DISK_SPACE;
    }
}

static uint32_t gdx_import_validate_direct(const char* name, const char* extension, int32_t contentType,
                                           uint8_t* payload, int32_t payloadSize, GdxContentImportEntry* entry,
                                           uint32_t* warnings, int encoding, int foreign) {
    uint32_t errors = 0;
    int32_t expectedSize;

    entry->contentType = contentType;
    entry->flags = 0;

    if (contentType != GDX_CONTENT_TYPE_TRACK && contentType != GDX_CONTENT_TYPE_MACHINE) {
        errors |= GDX_IMPORT_ERR_BAD_TYPE;
    }
    if (strcmp(extension, "CRSD") == 0 || strcmp(extension, "CRSE") == 0 || strcmp(extension, "CARD") == 0) {
        memcpy(entry->extension, extension, 5);
        entry->extension[5] = '\0';
    } else {
        entry->extension[0] = '\0';
        errors |= GDX_IMPORT_ERR_BAD_EXT;
    }
    /* Pairing: tracks carry CRSD/CRSE ('S' at [2]), machines carry CARD. */
    if ((errors & (GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT)) == 0 &&
        ((contentType == GDX_CONTENT_TYPE_TRACK) != (entry->extension[2] == 'S'))) {
        errors |= GDX_IMPORT_ERR_TYPE_EXT_MISMATCH;
    }

    snprintf(entry->name, sizeof(entry->name), "%s", name);
    if (Mfs_ValidateFileName(entry->name) != 0) {
        errors |= GDX_IMPORT_ERR_BAD_NAME;
    }

    expectedSize = (errors & (GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT)) == 0
                       ? gdx_import_expected_payload_size(contentType)
                       : -1;
    if (expectedSize < 0 || payloadSize != expectedSize) {
        errors |= GDX_IMPORT_ERR_BAD_SIZE;
    } else if (contentType == GDX_CONTENT_TYPE_TRACK) {
        if (foreign) {
            errors |= gdx_content_import_normalize_track(payload, payloadSize, encoding, payload, &entry->foreign);
        } else {
            gdx_import_validate_track(payload, &errors);
        }
    } else {
        gdx_import_validate_machine(payload, &errors);
    }

    /* Same gate as validate_all: live disk state is probed only when the identity checks passed. */
    if ((errors & (GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT | GDX_IMPORT_ERR_TYPE_EXT_MISMATCH |
                   GDX_IMPORT_ERR_BAD_NAME)) == 0) {
        gdx_import_check_disk_state(entry, &errors, warnings);
    } else {
        errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
    }
    return errors;
}

static void gdx_import_validate_payload(const char* name, const char* extension, int32_t contentType,
                                         const uint8_t* payload, int32_t payloadSize, GdxContentImportEntry* outEntry,
                                         int encoding, int foreign) {
    uint32_t warnings = 0;
    uint8_t* payloadCopy;

    if (outEntry == NULL) {
        return;
    }
    memset(outEntry, 0, sizeof(*outEntry));
    outEntry->classFileCount = -1;
    outEntry->bundleTrackCount = -1;
    if (name == NULL || name[0] == '\0' || strlen(name) > 20) {
        outEntry->errors = GDX_IMPORT_ERR_BAD_NAME | GDX_IMPORT_ERR_STATE_UNKNOWN;
        return;
    }
    if (extension == NULL) {
        outEntry->errors = GDX_IMPORT_ERR_BAD_EXT | GDX_IMPORT_ERR_STATE_UNKNOWN;
        return;
    }
    if (payload == NULL || payloadSize <= 0) {
        outEntry->errors = GDX_IMPORT_ERR_BAD_SIZE | GDX_IMPORT_ERR_STATE_UNKNOWN;
        return;
    }
    payloadCopy = (uint8_t*) malloc(payloadSize);
    if (payloadCopy == NULL) {
        outEntry->errors = GDX_IMPORT_ERR_IO | GDX_IMPORT_ERR_STATE_UNKNOWN;
        return;
    }
    memcpy(payloadCopy, payload, payloadSize);
    outEntry->errors =
        gdx_import_validate_direct(name, extension, contentType, payloadCopy, payloadSize, outEntry, &warnings, encoding, foreign);
    outEntry->warnings = warnings;
    free(payloadCopy);
}

static int gdx_import_begin_payload(const char* name, const char* extension, int32_t contentType,
                                     const uint8_t* payload, int32_t payloadSize, int encoding, int foreign) {
    GdxContentImportEntry entry;
    uint32_t warnings = 0;

    if (name == NULL || name[0] == '\0' || strlen(name) > 20 || extension == NULL || payload == NULL ||
        payloadSize <= 0 || payloadSize > GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    if (gdx_input_in_gameplay()) {
        return GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY;
    }
    if (sImportState == GDX_IMPORT_PENDING || sCupState == GDX_CUP_PENDING) {
        return GDX_CONTENT_IMPORT_ERR_PENDING;
    }
    if (gdx_content_mfs_busy() || gdx_recovery_blocks_writes()) {
        return GDX_CONTENT_ERR_BUSY;
    }

    sImportSnapshot = 0;
    memset(sImportBranch, 0, sizeof(sImportBranch));
    memset(&sImportExtInfo, 0, sizeof(sImportExtInfo));
    sImportTitle[0] = '\0';
    sImportSidecarBlock.count = 0;

    /* Re-validate what was handed in: never trust the menu's earlier snapshot. The payload then
     * stages straight into the buffer the disk thread installs. Copy first so the validator can
     * sanitize the staged bytes in-place (pit clamp + checksum rewrite). */
    sImportPayloadSize = (uint32_t) gdx_import_expected_payload_size(contentType);
    memcpy(sImportPayload, payload, payloadSize);

    memset(&entry, 0, sizeof(entry));
    entry.classFileCount = -1;
    entry.bundleTrackCount = -1;
    entry.errors =
        gdx_import_validate_direct(name, extension, contentType, sImportPayload, payloadSize, &entry, &warnings, encoding, foreign);
    if (entry.errors != 0) {
        return GDX_CONTENT_IMPORT_ERR_VALIDATION;
    }

    memcpy(sImportName, entry.name, sizeof(sImportName));
    memcpy(sImportExtension, entry.extension, sizeof(sImportExtension));
    sImportContentType = entry.contentType;
    sImportRc = 0;
    sImportMfsError = 0;
    sImportCupCleared = 0;
    sImportState = GDX_IMPORT_PENDING;

    if (GdxContentImport_EnqueueDiskOp() != 0) {
        sImportState = GDX_IMPORT_IDLE;
        return GDX_CONTENT_ERR_BUSY;
    }
    return GDX_CONTENT_OK;
}

void gdx_content_import_validate_payload(const char* name, const char* extension, int32_t type,
                                         const uint8_t* payload, int32_t size, GdxContentImportEntry* out) {
    gdx_import_validate_payload(name, extension, type, payload, size, out, GDX_FOREIGN_HOST_ORDER, 0);
}

int gdx_content_import_begin_payload(const char* name, const char* extension, int32_t type,
                                     const uint8_t* payload, int32_t size) {
    return gdx_import_begin_payload(name, extension, type, payload, size, GDX_FOREIGN_HOST_ORDER, 0);
}

void gdx_content_import_validate_foreign_payload(const char* name, const char* extension, int32_t type,
                                                const uint8_t* payload, int32_t size, int encoding,
                                                GdxContentImportEntry* out) {
    gdx_import_validate_payload(name, extension, type, payload, size, out, encoding, 1);
}

int gdx_content_import_begin_foreign_payload(const char* name, const char* extension, int32_t type,
                                             const uint8_t* payload, int32_t size, int encoding) {
    return gdx_import_begin_payload(name, extension, type, payload, size, encoding, 1);
}
