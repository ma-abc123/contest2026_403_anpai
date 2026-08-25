# AI Watch Board Configuration

This contest-owned board configuration targets the SiFli SF32LB52
`lckfb_huangshan_pi` hardware platform.

The `configs/ai_watch/defconfig` file enables the AI Watch application and
continues to use the existing SiFli board support package through:

```text
CONFIG_ARCH_BOARD_CUSTOM_DIR=../vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi
```

Build the configuration from the openvela workspace root:

```bash
./build.sh vendor/openvela/boards/contest2026_403_ai_watch/configs/ai_watch/
```

The application and configuration profile are owned by this contest repository.
Required changes to the existing SiFli board startup code remain separate
patches in `patches/vendor-sifli/` and are intended for a later public-repo PR.
