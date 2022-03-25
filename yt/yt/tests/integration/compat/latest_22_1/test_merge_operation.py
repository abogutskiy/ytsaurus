from original_tests.yt.yt.tests.integration.tests.controller.test_merge_operation \
    import TestSchedulerMergeCommands as BaseTestMergeCommands


class TestMergeCommandsCompatUpToCA(BaseTestMergeCommands):
    ARTIFACT_COMPONENTS = {
        "22_1": ["master", "node", "job-proxy", "exec", "tools"],
        "trunk": ["scheduler", "controller-agent", "proxy", "http-proxy"],
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "enable_table_column_renaming": False,
        },
    }


class TestMergeCommandsCompatNewNodes(BaseTestMergeCommands):
    UPLOAD_DEBUG_ARTIFACT_CHUNKS = True

    ARTIFACT_COMPONENTS = {
        "21_3": ["master", "scheduler", "controller-agent"],
        "trunk": ["node", "job-proxy", "exec", "tools", "proxy", "http-proxy"],
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "enable_table_column_renaming": False,
        },
    }
