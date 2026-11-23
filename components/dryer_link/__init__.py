"""Dryer Link: finds the dryer on either GE bus, learns what it has, and serves the setup walkthrough.

The walkthrough is setup.html next to this file. It is embedded gzipped, with its LIVE switch turned on so it
talks to the module instead of its built-in pretend dryer.
"""
import gzip
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import uart, update, web_server_base
from esphome.components.gea import CONF_GEA_ID, GEAComponent
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID, CONF_VERSION
from esphome.core import CORE

DEPENDENCIES = ["gea", "uart", "wifi", "captive_portal"]
AUTO_LOAD = ["web_server_base", "update"]

CONF_CLASSIC_UART = "classic_uart"
CONF_NEW_UART = "new_uart"
CONF_FEATURES = "features"
CONF_ERD = "erd"
CONF_LABEL = "label"
CONF_CONTROL = "control"
CONF_ENTITIES = "entities"
CONF_WRITE_CHECK = "write_check"
CONF_SETUP_ON_NETWORK = "setup_on_network"
CONF_STATUS_PAGE_LOGIN = "status_page_login"
CONF_UPDATE_ID = "update_id"
CONF_AUTO_UPDATE = "auto_update"

LIVE_OFF = "const LIVE = false;"
LIVE_ON = "const LIVE = true;"

# The 4 kB flash partition that holds this module's own setup password. tools/flash_module.py writes it when the
# module is flashed; firmware updates never touch it. Keep in sync with label_esp32.cpp and that tool.
LABEL_PARTITION = "dl_label"
LABEL_PARTITION_TYPE = 0x40
LABEL_PARTITION_SUBTYPE = 0x00
LABEL_PARTITION_SIZE = 0x1000

dryer_link_ns = cg.esphome_ns.namespace("dryer_link")
DryerLink = dryer_link_ns.class_("DryerLink", cg.Component)

FEATURE_SCHEMA = cv.Schema(
    {
        # The data point that carries this feature. If the dryer does not answer for it, the entities below are
        # hidden and it is no longer polled.
        cv.Required(CONF_ERD): cv.hex_uint16_t,
        # What the setup page calls it. Features that share a label show as one line.
        cv.Required(CONF_LABEL): cv.string_strict,
        # True for things the owner can do (listed separately on the page), false for things the dryer reports.
        cv.Optional(CONF_CONTROL, default=False): cv.boolean,
        # For an option the owner can switch: the data point a change is written to. While learning, the value the
        # option already has is written back there, unchanged; a dryer that does not acknowledge it does not
        # accept writes, and the feature is left out.
        cv.Optional(CONF_WRITE_CHECK): cv.hex_uint16_t,
        cv.Optional(CONF_ENTITIES, default=[]): cv.ensure_list(cv.use_id(cg.EntityBase)),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DryerLink),
        cv.GenerateID(CONF_GEA_ID): cv.use_id(GEAComponent),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
        # GEA2: 19200 baud, one wire. GEA3: 230400 baud, two wires.
        cv.Required(CONF_CLASSIC_UART): cv.use_id(uart.UARTComponent),
        cv.Required(CONF_NEW_UART): cv.use_id(uart.UARTComponent),
        cv.Required(CONF_VERSION): cv.string_strict,
        # At most 32: what was learned is stored as one bit per feature.
        cv.Required(CONF_FEATURES): cv.All(cv.ensure_list(FEATURE_SCHEMA), cv.Length(min=1, max=32)),
        # Serve the walkthrough and its API on the home network too, not only on the setup hotspot. For test builds.
        cv.Optional(CONF_SETUP_ON_NETWORK, default=False): cv.boolean,
        # Ask for the module's label password ("admin" + password) on the status page. Needs `web_server: auth:`
        # in the configuration, which only switches the feature on; the password itself comes from the module.
        cv.Optional(CONF_STATUS_PAGE_LOGIN, default=True): cv.boolean,
        # The update entity to install from, and whether to install by itself when a different version is offered.
        cv.Optional(CONF_UPDATE_ID): cv.use_id(update.UpdateEntity),
        cv.Optional(CONF_AUTO_UPDATE, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    web_server = fv.full_config.get().get("web_server")
    if config[CONF_STATUS_PAGE_LOGIN] and web_server is not None and "auth" not in web_server:
        raise cv.Invalid(
            f"'{CONF_STATUS_PAGE_LOGIN}' needs an 'auth:' block under 'web_server:' (any placeholder login; the module "
            f"replaces it with its own password). Add one, or set '{CONF_STATUS_PAGE_LOGIN}: false'."
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _page_gz() -> bytes:
    page = (Path(__file__).parent / "setup.html").read_text(encoding="utf-8")
    if page.count(LIVE_OFF) != 1:
        raise cv.Invalid(f"setup.html must contain '{LIVE_OFF}' exactly once")
    return gzip.compress(page.replace(LIVE_OFF, LIVE_ON).encode("utf-8"), compresslevel=9, mtime=0)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    hub = await cg.get_variable(config[CONF_GEA_ID])
    cg.add(hub.set_autostart(False))  # this component decides which UART and language the hub runs on
    cg.add(var.set_hub(hub))
    cg.add(var.set_web_server_base(await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])))
    cg.add(var.set_classic_uart(await cg.get_variable(config[CONF_CLASSIC_UART])))
    cg.add(var.set_new_uart(await cg.get_variable(config[CONF_NEW_UART])))
    cg.add(var.set_version(config[CONF_VERSION]))
    cg.add(var.set_setup_on_network(config[CONF_SETUP_ON_NETWORK]))
    cg.add(var.set_status_page_login(config[CONF_STATUS_PAGE_LOGIN]))
    cg.add(var.set_auto_update(config[CONF_AUTO_UPDATE]))
    if CONF_UPDATE_ID in config:
        cg.add(var.set_update(await cg.get_variable(config[CONF_UPDATE_ID])))

    for feature in config[CONF_FEATURES]:
        cg.add(
            var.add_feature(
                feature[CONF_ERD], feature[CONF_LABEL], feature[CONF_CONTROL], feature.get(CONF_WRITE_CHECK, 0)
            )
        )
        for entity_id in feature[CONF_ENTITIES]:
            cg.add(var.add_feature_entity(await cg.get_variable(entity_id)))

    if CORE.is_esp32:
        from esphome.components.esp32 import add_partition

        add_partition(LABEL_PARTITION, LABEL_PARTITION_TYPE, LABEL_PARTITION_SUBTYPE, LABEL_PARTITION_SIZE)

    gz = _page_gz()
    data = ", ".join(str(b) for b in gz)
    cg.add_global(cg.RawExpression(f"extern const uint8_t DRYER_LINK_PAGE_GZ[{len(gz)}] = {{{data}}}"))
    cg.add_global(cg.RawExpression(f"extern const size_t DRYER_LINK_PAGE_GZ_SIZE = {len(gz)}"))
