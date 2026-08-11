/*
  sys_can_tcan4550.c - MCU specific CAN code for a TI TCAN4550-Q1
  (Bosch M_CAN core + transceiver) attached over SPI.

  Used on boards whose MCU has no CAN peripheral (e.g. STM32F051).
  Ported from the proven Firefly GEN2 GPS node implementation
  (dronecan_gps_mag_stm32l011). The TCAN is serviced entirely by
  polling from the main loop (CAN_POLLED_RX) so no interrupt here can
  ever delay motor commutation.

  Requires these target defines:
    CAN_TCAN4550                 - selects this driver
    TCAN_SCK_, TCAN_MISO_, TCAN_MOSI_, TCAN_CS_ port/pin defines (targets.h)
 */

#include "targets.h"

#if DRONECAN_SUPPORT && defined(CAN_TCAN4550)

#include "peripherals.h"
#include "sys_can.h"
#include <string.h>

/* bring-up state: 0=not started, 3=running, 0xE1/0xE2/0xE3=failed stage
   (same codes as the GPS node for bench familiarity).
   volatile so -Os keeps them readable from a debugger. */
static volatile uint8_t can_state;
static volatile uint8_t can_ok;
static volatile uint32_t can_attempts;
static volatile uint32_t spi_timeouts;
static uint32_t rx_poll_countdown;

/* ---------------- SPI (STM32F0, SPI with 4-level FIFO) ---------------- */

static void cs_high(void) { TCAN_CS_PORT->BSRR = TCAN_CS_PIN; }
static void cs_low(void)  { TCAN_CS_PORT->BRR = TCAN_CS_PIN; }

/*
  configure one pin as AF0 push-pull high-speed. pin is the 0..15 pin index.
 */
static void pin_af0(GPIO_TypeDef *port, uint32_t pin)
{
    port->MODER = (port->MODER & ~(3u << (pin * 2))) | (2u << (pin * 2)); // AF mode
    port->OTYPER &= ~(1u << pin);                                         // push-pull
    port->OSPEEDR |= 3u << (pin * 2);                                     // high speed
    port->PUPDR &= ~(3u << (pin * 2));
    volatile uint32_t *afr = &port->AFR[pin >> 3];
    *afr &= ~(0xFu << ((pin & 7) * 4));                                   // AF0
}

static void spi_setup(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;

    SPI1->CR1 &= ~SPI_CR1_SPE; // safe to re-call on a bring-up retry

    // CS: plain GPIO output, idle high
    TCAN_CS_PORT->MODER = (TCAN_CS_PORT->MODER & ~(3u << (TCAN_CS_PINNUM * 2)))
                          | (1u << (TCAN_CS_PINNUM * 2));
    TCAN_CS_PORT->OTYPER &= ~(1u << TCAN_CS_PINNUM);
    TCAN_CS_PORT->OSPEEDR |= 3u << (TCAN_CS_PINNUM * 2);
    cs_high();

    pin_af0(TCAN_SCK_PORT, TCAN_SCK_PINNUM);
    pin_af0(TCAN_MISO_PORT, TCAN_MISO_PINNUM);
    pin_af0(TCAN_MOSI_PORT, TCAN_MOSI_PINNUM);

    // master, CPOL=0 CPHA=0, fPCLK/8 = 6 MHz (TCAN4550 max is 18 MHz),
    // software NSS held high
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_1;
    // 8-bit frames; FRXTH so RXNE fires per byte (F0 SPI has a 4-level FIFO)
    SPI1->CR2 = SPI_CR2_DS_0 | SPI_CR2_DS_1 | SPI_CR2_DS_2 | SPI_CR2_FRXTH;
    SPI1->CR1 |= SPI_CR1_SPE;
}

/*
  bounded SPI byte transfer: an unbounded TXE/RXNE spin hangs the MCU
  forever if the TCAN is unpowered or mid-POR. Bounding it returns 0 so
  bring-up fails cleanly and retries - it must never stall commutation.
 */
static inline uint8_t spi_byte(uint8_t o)
{
    uint32_t t = 1000;
    while (!(SPI1->SR & SPI_SR_TXE))
        if (--t == 0) { spi_timeouts++; return 0; }
    *(volatile uint8_t *)&SPI1->DR = o; // byte access - 16-bit access packs 2 bytes
    t = 1000;
    while (!(SPI1->SR & SPI_SR_RXNE))
        if (--t == 0) { spi_timeouts++; return 0; }
    return *(volatile uint8_t *)&SPI1->DR;
}

/* ---------------- TCAN4550 register access ---------------- */

#define TCAN_OP_WRITE 0x61
#define TCAN_OP_READ 0x41

static uint32_t tcan_read32(uint16_t addr)
{
    uint32_t v;
    cs_low();
    spi_byte(TCAN_OP_READ);
    spi_byte((uint8_t)(addr >> 8));
    spi_byte((uint8_t)(addr & 0xFF));
    spi_byte(1);
    v = (uint32_t)spi_byte(0) << 24;
    v |= (uint32_t)spi_byte(0) << 16;
    v |= (uint32_t)spi_byte(0) << 8;
    v |= (uint32_t)spi_byte(0);
    cs_high();
    return v;
}

static void tcan_write32(uint16_t addr, uint32_t data)
{
    cs_low();
    spi_byte(TCAN_OP_WRITE);
    spi_byte((uint8_t)(addr >> 8));
    spi_byte((uint8_t)(addr & 0xFF));
    spi_byte(1);
    spi_byte((uint8_t)(data >> 24));
    spi_byte((uint8_t)(data >> 16));
    spi_byte((uint8_t)(data >> 8));
    spi_byte((uint8_t)(data));
    cs_high();
}

/* TCAN register map (subset) */
#define TCAN_DEVICE_ID1 0x0000
#define TCAN_DEVICE_ID2 0x0004
#define TCAN_ID1_EXPECT 0x4E414354UL
#define TCAN_ID2_EXPECT 0x30353534UL
#define TCAN_DEV_MODE_CFG 0x0800
#define TCAN_DEV_IR 0x0820
#define TCAN_DEV_MCAN_IR 0x0824
#define MCAN_CCCR 0x1018
#define MCAN_NBTP 0x101C
#define MCAN_GFC 0x1080
#define MCAN_SIDFC 0x1084
#define MCAN_XIDFC 0x1088
#define MCAN_RXF0C 0x10A0
#define MCAN_RXF0S 0x10A4
#define MCAN_RXF0A 0x10A8
#define MCAN_RXBC 0x10AC
#define MCAN_RXF1C 0x10B0
#define MCAN_RXESC 0x10BC
#define MCAN_TXBC 0x10C0
#define MCAN_TXESC 0x10C8
#define MCAN_TXBRP 0x10CC
#define MCAN_TXBAR 0x10D0

#define MODE_SEL_MASK (3u << 6)
#define MODE_SEL_STANDBY (1u << 6)
#define MODE_SEL_NORMAL (2u << 6)
#define DEV_CLK_REF_40MHZ (1u << 27)
#define DEV_WD_EN (1u << 3)
#define DEV_SWE_DIS (1u << 0)
#define CCCR_INIT (1u << 0)
#define CCCR_CCE (1u << 1)

#define MRAM_BASE 0x8000
#define MRAM_WORDS 512
/* MRAM layout (each element = 16 B for 8-byte data):
   TX buffer 0 @ 0x000 (1 elem); RX FIFO 0 @ 0x010 (6 elems) */
#define TXBUF0_ADDR (MRAM_BASE + 0x000)
#define RXF0_OFFSET 0x010
#define RXF0_ELEMS 6
#define RXF0_ELEM_BYTES 16
#define RXF0_ADDR(i) (MRAM_BASE + RXF0_OFFSET + (i)*RXF0_ELEM_BYTES)

#ifndef TCAN_NBTP_1MBIT
// 1 Mbit/s from the TCAN4550's 40 MHz crystal
#define TCAN_NBTP_1MBIT 0x0E001E07UL
#endif

/* MCAN_CCCR is in the crystal clock domain and reads all-zero until the
   oscillator starts (several ms at cold boot); bound the spin */
#define TCAN_CFG_TIMEOUT 2000u

static int tcan_configure_1mbit_classic(void)
{
    uint32_t cfg;

    for (uint32_t i = 0; i < MRAM_WORDS; i++) {
        tcan_write32(MRAM_BASE + i * 4, 0);
    }

    cfg = tcan_read32(TCAN_DEV_MODE_CFG);
    cfg &= ~MODE_SEL_MASK;
    cfg |= MODE_SEL_STANDBY;
    cfg &= ~DEV_WD_EN;
    cfg |= DEV_SWE_DIS;
    cfg |= DEV_CLK_REF_40MHZ;
    tcan_write32(TCAN_DEV_MODE_CFG, cfg);

    tcan_write32(MCAN_CCCR, CCCR_INIT);
    for (uint32_t t = TCAN_CFG_TIMEOUT; !(tcan_read32(MCAN_CCCR) & CCCR_INIT);) {
        if (--t == 0) {
            return -1; // M_CAN clock domain dark (crystal not up yet)
        }
    }
    tcan_write32(MCAN_CCCR, CCCR_INIT | CCCR_CCE);

    tcan_write32(MCAN_NBTP, TCAN_NBTP_1MBIT);

    /* no filter lists; GFC=0 accepts all frames into RX FIFO 0 and
       libcanard's shouldAcceptTransfer discards what we don't want */
    tcan_write32(MCAN_GFC, 0);
    tcan_write32(MCAN_SIDFC, 0);
    tcan_write32(MCAN_XIDFC, 0);
    tcan_write32(MCAN_RXF0C, ((uint32_t)RXF0_ELEMS << 16) | RXF0_OFFSET);
    tcan_write32(MCAN_RXF1C, 0);
    tcan_write32(MCAN_RXBC, 0);
    tcan_write32(MCAN_RXESC, 0); // 8-byte RX + TX elements
    tcan_write32(MCAN_TXESC, 0);
    tcan_write32(MCAN_TXBC, (1u << 16) | 0);

    tcan_write32(MCAN_CCCR, 0); // clear INIT/CCE -> start M_CAN core

    tcan_write32(TCAN_DEV_IR, 0xFFFFFFFFUL);
    tcan_write32(TCAN_DEV_MCAN_IR, 0xFFFFFFFFUL);

    cfg = tcan_read32(TCAN_DEV_MODE_CFG);
    cfg &= ~MODE_SEL_MASK;
    cfg |= MODE_SEL_NORMAL;
    tcan_write32(TCAN_DEV_MODE_CFG, cfg);

    return 0;
}

static void can_bringup(void)
{
    can_attempts++;
    /* re-init SPI each attempt: a cold boot can leave it wedged from
       talking to the TCAN too early */
    spi_setup();
    if (tcan_read32(TCAN_DEVICE_ID1) != TCAN_ID1_EXPECT ||
        tcan_read32(TCAN_DEVICE_ID2) != TCAN_ID2_EXPECT) {
        can_state = 0xE1; // SPI/CS bad or TCAN unpowered -> retry later
        can_ok = 0;
        return;
    }
    if (tcan_configure_1mbit_classic() != 0) {
        can_state = 0xE2; // crystal/M_CAN clock not up yet -> retry later
        can_ok = 0;
        return;
    }
    /* a successful config does NOT prove the bus is live: below the UVSUP
       release threshold the transceiver stays latched in STANDBY (config
       reads back fine but the node is dark). Verify MODE_SEL actually
       reads NORMAL; if not, keep retrying until the rail settles. */
    if ((tcan_read32(TCAN_DEV_MODE_CFG) & MODE_SEL_MASK) != MODE_SEL_NORMAL) {
        can_state = 0xE3;
        can_ok = 0;
        return;
    }
    can_state = 3;
    can_ok = 1;
}

/* ---------------- sys_can.h API ---------------- */

void sys_can_init(void)
{
    can_bringup();
}

/*
  polled transport: nothing here may block, so IRQ enable/disable are no-ops
 */
void sys_can_disable_IRQ(void) {}
void sys_can_enable_IRQ(void) {}

int16_t sys_can_transmit(const CanardCANFrame *txf)
{
    if (!can_ok) {
        return -1; // drop the frame; bring-up retry happens in receive path
    }
    if (tcan_read32(MCAN_TXBRP) & 1u) {
        return 0; // single TX buffer still busy - caller retries next update
    }
    // T0: XTD=1, RTR=0, ESI=0, 29-bit ID (DroneCAN is always extended)
    tcan_write32(TXBUF0_ADDR + 0x0, (1u << 30) | (txf->id & 0x1FFFFFFF));
    // T1: DLC in [19:16], classic CAN
    tcan_write32(TXBUF0_ADDR + 0x4, ((uint32_t)(txf->data_len & 0xF)) << 16);
    uint32_t w2 = 0, w3 = 0;
    for (uint8_t i = 0; i < 4 && i < txf->data_len; i++) {
        w2 |= (uint32_t)txf->data[i] << (8 * i);
    }
    for (uint8_t i = 4; i < 8 && i < txf->data_len; i++) {
        w3 |= (uint32_t)txf->data[i] << (8 * (i - 4));
    }
    tcan_write32(TXBUF0_ADDR + 0x8, w2);
    tcan_write32(TXBUF0_ADDR + 0xC, w3);
    tcan_write32(MCAN_TXBAR, 1u);
    canstats.num_tx_interrupts++;
    return 1;
}

int16_t sys_can_receive(CanardCANFrame *rx_frame)
{
    if (!can_ok) {
        // lazy bring-up retry (TCAN powered later than MCU, UVSUP, etc).
        // Retry roughly every 4096 polls so a dead TCAN costs ~one SPI
        // transaction per poll, not a full bring-up per poll.
        if (rx_poll_countdown++ >= 4096) {
            rx_poll_countdown = 0;
            can_bringup();
        }
        return 0;
    }

    uint32_t s = tcan_read32(MCAN_RXF0S);
    uint32_t fill = s & 0x7F;
    if (fill == 0) {
        return 0;
    }
    uint32_t gi = (s >> 8) & 0x3F;
    uint16_t ea = RXF0_ADDR(gi);

    uint32_t r0 = tcan_read32(ea + 0x0); // [31]ESI [30]XTD [29]RTR [28:0]ID
    uint32_t r1 = tcan_read32(ea + 0x4); // [19:16]DLC
    uint32_t d0 = tcan_read32(ea + 0x8);
    uint32_t d1 = tcan_read32(ea + 0xC);

    uint8_t xtd = (r0 >> 30) & 1u;
    uint32_t id = xtd ? (r0 & 0x1FFFFFFFu) : ((r0 >> 18) & 0x7FFu);
    uint8_t dlc = (r1 >> 16) & 0xFu;
    if (dlc > 8) {
        dlc = 8;
    }

    rx_frame->id = id | (xtd ? CANARD_CAN_FRAME_EFF : 0u);
    rx_frame->data_len = dlc;
    rx_frame->data[0] = (uint8_t)d0;
    rx_frame->data[1] = (uint8_t)(d0 >> 8);
    rx_frame->data[2] = (uint8_t)(d0 >> 16);
    rx_frame->data[3] = (uint8_t)(d0 >> 24);
    rx_frame->data[4] = (uint8_t)d1;
    rx_frame->data[5] = (uint8_t)(d1 >> 8);
    rx_frame->data[6] = (uint8_t)(d1 >> 16);
    rx_frame->data[7] = (uint8_t)(d1 >> 24);

    tcan_write32(MCAN_RXF0A, gi); // acknowledge -> free the element
    canstats.num_rx_interrupts++;
    return 1;
}

/*
  16 byte unique ID from the STM32F0 96-bit UID, zero padded
 */
void sys_can_getUniqueID(uint8_t id[16])
{
    const uint8_t *uid = (const uint8_t *)0x1FFFF7AC;
    memcpy(id, uid, 12);
    memset(&id[12], 0, 4);
}

/*
  the F051 head-start build has no CAN bootloader, so the firmware-update
  handshake registers are RAM-backed stubs (read by nothing after reset)
 */
static uint32_t fake_rtc_backup[4];

uint32_t get_rtc_backup_register(uint8_t idx)
{
    return fake_rtc_backup[idx & 3];
}

void set_rtc_backup_register(uint8_t idx, uint32_t value)
{
    fake_rtc_backup[idx & 3] = value;
}

#endif // DRONECAN_SUPPORT && CAN_TCAN4550
