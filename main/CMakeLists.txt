idf_component_register(SRCS "main.c" "ota_server.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "certs/cacert.pem"
                                   "certs/prvtkey.pem"
                                   "certs/ca_cert.pem")
