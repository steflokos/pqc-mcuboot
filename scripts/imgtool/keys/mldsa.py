"""
ML-DSA (FIPS 204 post-quantum) key management
"""

# SPDX-License-Identifier: Apache-2.0

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import mldsa

from .general import KeyClass


class MLDSAUsageError(Exception):
    pass


# (private key class, public key class, sig_tlv/sig_type name, sig length)
_LEVELS = {
    44: (mldsa.MLDSA44PrivateKey, mldsa.MLDSA44PublicKey, 2420),
    65: (mldsa.MLDSA65PrivateKey, mldsa.MLDSA65PublicKey, 3309),
    87: (mldsa.MLDSA87PrivateKey, mldsa.MLDSA87PublicKey, 4627),
}


class MLDSAPublic(KeyClass):
    """Wrapper around an ML-DSA public key. Concrete per-level classes
    (MLDSA44Public/MLDSA65Public/MLDSA87Public) set _level."""

    _level = None

    def __init__(self, key):
        self.key = key

    def shortname(self):
        # Deliberately level-independent (unlike sig_tlv()/sig_type()):
        # only one ML-DSA level is ever compiled into a given bootloader
        # build (mutually-exclusive Kconfig choice), and boot/zephyr/keys.c
        # declares a single `extern const unsigned char mldsa_pub_key[]`
        # regardless of level -- matching the "ecdsa" (not "ecdsa256")
        # precedent in keys/ecdsa.py for the same reason.
        return "mldsa"

    def _unsupported(self, name):
        raise MLDSAUsageError(f"Operation {name} requires private key")

    def _get_public(self):
        return self.key

    def get_public_bytes(self):
        # The key is embedded into MCUboot in "SubjectPublicKeyInfo" format,
        # matching the ECDSA/ED25519 key classes.
        return self._get_public().public_bytes(
                encoding=serialization.Encoding.DER,
                format=serialization.PublicFormat.SubjectPublicKeyInfo)

    def get_public_pem(self):
        return self._get_public().public_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PublicFormat.SubjectPublicKeyInfo)

    def get_private_bytes(self, minimal, format):
        self._unsupported('get_private_bytes')

    def export_private(self, path, passwd=None):
        self._unsupported('export_private')

    def export_public(self, path):
        """Write the public key to the given file."""
        pem = self._get_public().public_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PublicFormat.SubjectPublicKeyInfo)
        with open(path, 'wb') as f:
            f.write(pem)

    def sig_type(self):
        return f"MLDSA{self._level}"

    def sig_tlv(self):
        return f"MLDSA{self._level}"

    def sig_len(self):
        return _LEVELS[self._level][2]

    def verify_digest(self, signature, digest):
        """Verify that signature is valid for given digest"""
        k = self.key
        if isinstance(self.key, _LEVELS[self._level][0]):
            k = self.key.public_key()
        return k.verify(signature=signature, data=digest)


class MLDSA(MLDSAPublic):
    """
    Wrapper around an ML-DSA private key. Concrete per-level classes
    (MLDSA44/MLDSA65/MLDSA87) set _level and provide generate().
    """

    def __init__(self, key):
        """key should be an instance of one of the MLDSA*PrivateKey classes"""
        self.key = key

    def _get_public(self):
        return self.key.public_key()

    def get_private_bytes(self, minimal, format):
        raise MLDSAUsageError(
            f"Operation not supported with {self.shortname()} keys")

    def export_private(self, path, passwd=None):
        """
        Write the private key to the given file, protecting it with the
        optional password.
        """
        if passwd is None:
            enc = serialization.NoEncryption()
        else:
            enc = serialization.BestAvailableEncryption(passwd)
        pem = self.key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.PKCS8,
                encryption_algorithm=enc)
        with open(path, 'wb') as f:
            f.write(pem)

    def sign_digest(self, digest):
        """Return the actual signature"""
        return self.key.sign(data=digest)


class MLDSA44Public(MLDSAPublic):
    _level = 44


class MLDSA65Public(MLDSAPublic):
    _level = 65


class MLDSA87Public(MLDSAPublic):
    _level = 87


class MLDSA44(MLDSA):
    _level = 44

    @staticmethod
    def generate():
        return MLDSA44(mldsa.MLDSA44PrivateKey.generate())


class MLDSA65(MLDSA):
    _level = 65

    @staticmethod
    def generate():
        return MLDSA65(mldsa.MLDSA65PrivateKey.generate())


class MLDSA87(MLDSA):
    _level = 87

    @staticmethod
    def generate():
        return MLDSA87(mldsa.MLDSA87PrivateKey.generate())
