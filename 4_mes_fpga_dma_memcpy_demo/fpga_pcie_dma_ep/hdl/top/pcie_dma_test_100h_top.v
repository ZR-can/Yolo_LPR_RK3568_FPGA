// Thin project-level top for the MES2L100H PCIe DMA/PIO endpoint.
// The vendor reference top is kept as pcie_dma_test to avoid changing the
// proven PCIe/DMA implementation.

module pcie_dma_test_100h_top (
    input              button_rst_n,

    // PCIe Gen2 x2 endpoint interface.
    input              ref_clk_p,
    input              ref_clk_n,
    input              perst_n,
    input      [1:0]   rxn,
    input      [1:0]   rxp,
    output     [1:0]   txn,
    output     [1:0]   txp,

    // Link/activity LEDs from the reference design.
    output             ref_led,
    output             pclk_led,

    // External UART for uart2apb configuration.
    output             txd,
    input              rxd
);

pcie_dma_test u_pcie_dma_test (
    .button_rst_n(button_rst_n),
    .ref_clk_p   (ref_clk_p),
    .ref_clk_n   (ref_clk_n),
    .perst_n     (perst_n),
    .rxn         (rxn),
    .rxp         (rxp),
    .txn         (txn),
    .txp         (txp),
    .ref_led     (ref_led),
    .pclk_led    (pclk_led),
    .txd         (txd),
    .rxd         (rxd)
);

endmodule
