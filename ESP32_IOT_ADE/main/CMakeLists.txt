idf_component_register(
    SRCS
        "main.c"
        "uart_parser.c"
        "web_server.c"
        "relay_logic.c"
        "status_led.c"
        "remote_server.c"
    INCLUDE_DIRS "."
    EMBED_FILES  "index.html"
)
