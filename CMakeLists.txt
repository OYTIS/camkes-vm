#
# Copyright 2018, Data61
# Commonwealth Scientific and Industrial Research Organisation (CSIRO)
# ABN 41 687 119 230.
#
# This software may be distributed and modified according to the terms of
# the BSD 2-Clause license. Note that NO WARRANTY is provided.
# See "LICENSE_BSD2.txt" for details.
#
# @TAG(DATA61_BSD)
#

cmake_minimum_required(VERSION 3.8.2)

set(configure_string "")

config_option(
    CAmkESVMGuestDMAIommu
    CAMKES_VM_GUEST_DMA_IOMMU
    "Enable DMA through the IOMMU"
    DEFAULT
    ON
)

# Create VMM configuration library
add_config_library(camkes_vmm "${configure_string}")

# Common VM build definitions
CAmkESAddImportPath(components)
CAmkESAddImportPath(interfaces)
CAmkESAddTemplatesPath(templates)
CAmkESAddTemplatesPath(components/Init/templates)

# Add VM Components and Libraries
add_subdirectory(components/Echo)
add_subdirectory(components/Firewall)
add_subdirectory(components/StringReverse)
add_subdirectory(components/UDPServer)

# Declare connectors with templates.
# We can rely on the naming scheme being consistent to allow for easier declaration.
foreach(
    connector
    IN
    ITEMS
    seL4UDPSend
    seL4UDPRecv
    seL4PicoUDPSend
    seL4PicoUDPRecv
    seL4VMNet
    seL4MultiSharedData
)
    DeclareCAmkESConnector(
        ${connector}
        FROM
        ${connector}-from.template.c
        TO
        ${connector}-to.template.c
    )
endforeach()
