"""CyberRemesher local bridge — reference Python client.

DCC plugins (the Blender addon and others) consume this client rather than
speaking raw sockets. See :class:`cyberbridge.client.Client`.
"""

from .client import BridgeError, Client, PROTOCOL_VERSION, load_obj, save_obj

__all__ = ["Client", "BridgeError", "PROTOCOL_VERSION", "load_obj", "save_obj"]
__version__ = "0.1.1"
