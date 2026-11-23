import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_ID, CONF_OPTIONS
from .. import (
    gea_ns,
    GEAComponent,
    CONF_GEA_ID,
    CONF_ERD,
    CONF_DECODE,
    CONF_BYTE_OFFSET,
    CONF_WRITE_ERD,
    CONF_DATA_SIZE,
    DECODE_TYPES,
    validate_options,
)

DEPENDENCIES = ["gea"]

GEASelect = gea_ns.class_("GEASelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(GEASelect)
    .extend(
        {
            cv.GenerateID(CONF_GEA_ID): cv.use_id(GEAComponent),
            cv.Required(CONF_ERD): cv.hex_uint16_t,
            cv.Required(CONF_OPTIONS): validate_options,
            cv.Optional(CONF_DECODE, default="uint8"): cv.enum(
                DECODE_TYPES, lower=True
            ),
            cv.Optional(CONF_BYTE_OFFSET, default=0): cv.uint8_t,
            cv.Optional(CONF_WRITE_ERD): cv.hex_uint16_t,
            cv.Optional(CONF_DATA_SIZE): cv.uint8_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    options_map = config[CONF_OPTIONS]
    option_labels = list(options_map.values())

    var = await select.new_select(config, options=option_labels)
    await cg.register_component(var, config)

    hub = await cg.get_variable(config[CONF_GEA_ID])
    cg.add(var.set_erd(config[CONF_ERD]))
    cg.add(var.set_decode(config[CONF_DECODE]))
    cg.add(var.set_byte_offset(config[CONF_BYTE_OFFSET]))

    if CONF_WRITE_ERD in config:
        cg.add(var.set_write_erd(config[CONF_WRITE_ERD]))
    if CONF_DATA_SIZE in config:
        cg.add(var.set_data_size(config[CONF_DATA_SIZE]))

    for key, label in options_map.items():
        cg.add(var.add_option(key, label))

    cg.add(hub.register_entity(var))
