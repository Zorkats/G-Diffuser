int gdx_course_branch_editor_saved(void) {
    char courseId[32];
    char reason[EDITOR_LABEL_CAP];
    char text[EDITOR_LABEL_CAP + 32];

    if (!gdx_course_state_edit_file_id(courseId, sizeof(courseId))) {
        return 0;
    }
    editor_export();
    if (gdx_branch_edit_commit(courseId, sPoints, D_802CB6D0.controlPointCount, COURSE_CONTEXT()->courseData.checksum,
                               reason, sizeof(reason)) != 0) {
        sprintf(text, "Forks not saved: %s", reason);
        gdx_course_edit_flash_message(text);
        return 0;
    }
    if (gdx_course_state_flush() < 0) {
        gdx_course_edit_flash_message("Course save cancelled: settings could not be saved");
        return 0;
    }
    return 1;
}
