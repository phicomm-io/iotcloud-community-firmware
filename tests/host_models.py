"""Strict product selection shared by host builds and isolated MQTT tests."""
import os

DEFAULT_MODELS = ("M1", "A1", "TC1")
SUPPORTED_MODELS = frozenset(DEFAULT_MODELS)


def selected_models(value=None):
    if value is None:
        value = os.environ.get("PHICOMM_TEST_MODELS")
    if value is None:
        return DEFAULT_MODELS

    tokens = value.split(",")
    if not value or any(not token for token in tokens):
        raise ValueError("PHICOMM_TEST_MODELS must contain one or more comma-separated models")
    if len(tokens) != len(set(tokens)):
        raise ValueError("PHICOMM_TEST_MODELS contains a duplicate model")
    unknown = [token for token in tokens if token not in SUPPORTED_MODELS]
    if unknown:
        raise ValueError("PHICOMM_TEST_MODELS contains unknown model(s): " + ",".join(unknown))
    return tuple(tokens)


if __name__ == "__main__":
    try:
        print("\n".join(selected_models()))
    except ValueError as exc:
        raise SystemExit(str(exc))
