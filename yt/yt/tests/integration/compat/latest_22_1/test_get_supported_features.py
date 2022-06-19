from original_tests.yt.yt.tests.integration.tests.misc.test_get_supported_features \
    import TestGetFeatures as BaseTestGetFeatures


class TestGetFeaturesNewProxy(BaseTestGetFeatures):
    ARTIFACT_COMPONENTS = {
        "22_1": ["master", "node", "job-proxy", "exec", "tools", "scheduler", "controller-agent"],
        "trunk": ["proxy", "http-proxy"],
    }
    SKIP_STATISTICS_DESCRIPTIONS = True
