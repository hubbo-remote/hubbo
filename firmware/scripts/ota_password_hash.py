# Build-time OTA credential: compiles in SHA-256(HUBBO_OTA_PASSWORD), never the
# password itself (ArduinoOTA.setPasswordHash). Unset or empty: OTA off.
#
# Never prints or logs the password or its hash. Note: ArduinoOTA's challenge
# uses the hash as the key, so an image (or a flash dump) with OTA compiled in
# grants OTA access to boards running it; treat such images as sensitive.
import hashlib
import os

Import("env")  # noqa: F821 (SCons)

password = os.environ.get("HUBBO_OTA_PASSWORD", "")
digest = hashlib.sha256(password.encode("utf-8")).hexdigest() if password else ""
env.Append(CPPDEFINES=[("HUBBO_OTA_PASSWORD_SHA256", env.StringifyMacro(digest))])  # noqa: F821
