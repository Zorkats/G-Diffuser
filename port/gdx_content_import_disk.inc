void GdxContentImport_RunOnDiskThread(void) {
    int32_t rc;
    int32_t cupCleared = 0;
    int32_t cupSlot = -1;
    int32_t row;
    char previousCup[sizeof(gEditCupTrackNames)];

    if (!gdx_snapshot_begin(1)) {
        sImportCupRegister = 0;
        sImportRc = -1;
        sImportState = GDX_IMPORT_DONE;
        return;
    }
    if (sImportCupRegister) {
        /* G-NET "Save to my tracks": the first free Edit Cup slot, chosen here on the thread that owns
         * gEditCupTrackNames. With none free nothing is written at all (plan decision 14). */
        for (row = 0; row < 6 && cupSlot < 0; row++) {
            if (gEditCupTrackNames[row][0] == '\0') {
                cupSlot = row;
            }
        }
        if (cupSlot < 0) {
            sImportCupFull = 1;
            rc = -1;
            goto done;
        }
    }
    if (sImportContentType == GDX_CONTENT_TYPE_TRACK && !gdx_import_prepare_snapshot(-1)) {
        rc = -1;
        goto done;
    }
    if (sImportContentType == GDX_CONTENT_TYPE_TRACK) {
        rc = Mfs_DeleteFileInDir(gWorkingDirectory, sImportName,
                                 (char*) (sImportExtension[3] == 'D' ? "CRSE" : "CRSD"), 1);
        // A missing twin is expected; other failures must not be hidden by the save.
        if (rc != 0 && gMfsError != 0xF2) {
            goto done;
        }
    }

    rc = Mfs_SaveFile(gWorkingDirectory, sImportName, sImportExtension, sImportPayload, sImportPayloadSize, 0, 0xFF,
                      1);

    if (rc == 0 && sImportContentType == GDX_CONTENT_TYPE_TRACK &&
        !gdx_snapshot_import_restore(gWorkingDirectory, sImportName, sImportExtension, sImportPayload)) {
        rc = -1;
    }
    if (rc == 0 && sImportContentType == GDX_CONTENT_TYPE_TRACK) {
        gdx_import_store_meta(sImportName, sImportExtension, 0, sImportPayload);
    }
    if (rc == 0 && cupSlot >= 0) {
        /* The track is on the disk; the cup names it the way gdx_content_cup_register_begin does. A failed
         * CENT write keeps the old cup, and the track stays saved outside it. */
        memcpy(previousCup, gEditCupTrackNames, sizeof(previousCup));
        memset(gEditCupTrackNames[cupSlot], 0, sizeof(gEditCupTrackNames[cupSlot]));
        memcpy(gEditCupTrackNames[cupSlot], sImportName, strlen(sImportName));
        rc = Mfs_SaveFile(gWorkingDirectory, "CRS_ENTRY", "CENT", (uint8_t*) gEditCupTrackNames,
                          sizeof(gEditCupTrackNames), 0, 0xFF, 1);
        if (rc != 0) {
            memcpy(gEditCupTrackNames, previousCup, sizeof(previousCup));
        }
    }

    if (rc == 0 && sImportContentType == GDX_CONTENT_TYPE_TRACK && sImportExtension[3] == 'E') {
        /* CRSE import: clear edit-cup slots naming it, mirroring the editor's post-save clear
         * (19DD60.c case 0x12 -> func_xk2_800EBFE8) and persist CRS_ENTRY.CENT like
         * func_xk2_800EC110. The write happens only when a slot actually matched -- an
         * all-zero (never-loaded) in-memory cup can never match a validated non-empty name, so
         * a stale gEditCupTrackNames is never written back over the user's CENT. */
        int i;
        memcpy(previousCup, gEditCupTrackNames, sizeof(previousCup));
        for (i = 0; i < 6; i++) {
            char slot[GDX_EDIT_CUP_NAME_SIZE + 1];
            memcpy(slot, gEditCupTrackNames[i], GDX_EDIT_CUP_NAME_SIZE);
            slot[GDX_EDIT_CUP_NAME_SIZE] = '\0';
            if (slot[0] != '\0' && strcmp(slot, sImportName) == 0) {
                gEditCupTrackNames[i][0] = '\0';
                cupCleared++;
            }
        }
        if (cupCleared > 0) {
            rc = Mfs_SaveFile(gWorkingDirectory, "CRS_ENTRY", "CENT", (uint8_t*) gEditCupTrackNames,
                              sizeof(gEditCupTrackNames), 0, 0xFF, 1);
            if (rc != 0) {
                memcpy(gEditCupTrackNames, previousCup, sizeof(previousCup));
                cupCleared = 0;
            }
        }
    }

done:
    gdx_snapshot_end();
    sImportCupRegister = 0;
    sImportCupCleared = cupCleared;
    sImportMfsError = gMfsError;
    sImportRc = rc;
    sImportState = GDX_IMPORT_DONE;
}

void GdxContentBundle_RunOnDiskThread(void) {
    int32_t rc = 0;
    int32_t failedIndex = -1;
    int32_t i;
    char previousCup[sizeof(gEditCupTrackNames)];

    if (!gdx_snapshot_begin(1)) {
        sImportRc = -1;
        sImportState = GDX_IMPORT_DONE;
        return;
    }
    for (i = 0; i < sBundleTrackCount; ++i) {
        if (!gdx_import_prepare_snapshot(i)) {
            rc = -1;
            failedIndex = 0;
            goto done;
        }
    }
    {
        const char* names[GDX_CONTENT_BUNDLE_MAX_TRACKS];
        const void* bases[GDX_CONTENT_BUNDLE_MAX_TRACKS];
        for (i = 0; i < sBundleTrackCount; ++i) {
            names[i] = sBundleNames[i];
            bases[i] = sBundlePayloads[i];
        }
        if (!gdx_snapshot_import_check(gWorkingDirectory, names, bases, sBundleTrackCount)) {
            rc = -1;
            failedIndex = 0;
            goto done;
        }
    }
    for (i = 0; i < sBundleTrackCount; i++) {
        rc = Mfs_DeleteFileInDir(gWorkingDirectory, sBundleNames[i], "CRSE", 1);
        if (rc != 0 && gMfsError != 0xF2) {
            failedIndex = i;
            break;
        }
        rc = Mfs_SaveFile(gWorkingDirectory, sBundleNames[i], "CRSD", sBundlePayloads[i],
                          GDX_CONTENT_TRACK_PAYLOAD_SIZE, 0, 0xFF, 1);
        if (rc == 0 && !gdx_snapshot_import_restore(gWorkingDirectory, sBundleNames[i], "CRSD", sBundlePayloads[i])) {
            rc = -1;
        }
        if (rc != 0) {
            failedIndex = i;
            break;
        }
        gdx_import_store_meta(sBundleNames[i], "CRSD", i, sBundlePayloads[i]);
    }
    if (rc == 0) {
        memcpy(previousCup, gEditCupTrackNames, sizeof(previousCup));
        /* Only the 6 Edit-Cup rows merge; rows 6..23 of the in-memory image hold the importer's
         * other cups and keep their state. */
        for (i = 0; i < GDX_CONTENT_CUP_SLOT_COUNT; i++) {
            memcpy(gEditCupTrackNames[i], sBundleCent + i * GDX_EDIT_CUP_NAME_SIZE, GDX_EDIT_CUP_NAME_SIZE);
        }
        rc = Mfs_SaveFile(gWorkingDirectory, "CRS_ENTRY", "CENT", (uint8_t*) gEditCupTrackNames,
                          sizeof(gEditCupTrackNames), 0, 0xFF, 1);
        if (rc != 0) {
            memcpy(gEditCupTrackNames, previousCup, sizeof(previousCup));
            failedIndex = sBundleTrackCount;
        }
    }

done:
    gdx_snapshot_end();
    sBundleFailedIndex = failedIndex;
    sImportMfsError = gMfsError;
    sImportRc = rc;
    sImportState = GDX_IMPORT_DONE;
}
