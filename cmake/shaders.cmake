separate_arguments(FILES)

foreach (file ${FILES})
    set(filename ${SOURCE_DIR}/${file})
    get_filename_component(name ${filename} NAME_WE)

    set(source "")

    file(STRINGS ${filename} lines)
    foreach (line ${lines})
        set(source "${source}    \"${line}\\n\"\n")
    endforeach()

    file(
        WRITE ${DESTINATION_DIR}/${file}.h
        "static const GLchar *${name}ShaderSource[] = {\n"
        "${source}"
        "};\n"
    )
endforeach()
