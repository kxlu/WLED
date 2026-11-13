// LAN9303 Virtual PHY driver for LC3208, ESP-IDF 4.4 / Arduino ESP32 2.x
// Datasheet: SMSC LAN9303 Rev 1.5, sections 7.3, 13.2.6.
// Only manages CPU-facing Port 0 Virtual PHY; Port 1/2 are separate PHYs.
#include "esp_eth_phy_lan9303.h"
#include <cstdlib>
#include <cstdint>
#include <sys/cdefs.h>
#include "esp_eth.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// 0 = Disabled
// 1 = ESP_LOGI
// 2 = Serial.printf
#define LAN9303_DEBUG_MODE 0

#if LAN9303_DEBUG_MODE == 1
    #define LAN9303_DEBUG(fmt, ...) \
        ESP_LOGI("lan9303", fmt, ##__VA_ARGS__)
#elif LAN9303_DEBUG_MODE == 2
    #include <Arduino.h>
    #define LAN9303_DEBUG(fmt, ...) \
        Serial.printf("[LAN9303] " fmt "\n", ##__VA_ARGS__)
#else
    #define LAN9303_DEBUG(fmt, ...) do {} while (0)
#endif

namespace {
constexpr uint32_t BMCR=0, BMSR=1, ID1=2, ID2=3, ANAR=4, ANLPAR=5, SPECIAL=31;
constexpr uint32_t RESET=0x8000, LOOPBACK=0x4000, SPEED100=0x2000;
constexpr uint32_t AN_ENABLE=0x1000, AN_RESTART=0x0200, FULL_DUPLEX=0x0100;
constexpr uint32_t BMSR_LINK=0x0004, BMSR_AN_COMPLETE=0x0020;
constexpr uint32_t SPECIAL_MODE_MASK=0x0300, SPECIAL_RMII_PHY=0x0200;
constexpr uint32_t SPECIAL_CLOCK_OUT=0x0040, SPECIAL_TURBO=0x0400;
constexpr uint32_t SPECIAL_SPEED_DUPLEX_MASK=0x001c;
constexpr uint32_t ANAR_SELECTOR=0x0001;
struct Device {
    esp_eth_phy_t parent;
    esp_eth_mediator_t *mediator;
    uint32_t addr, reset_timeout_ms, autonego_timeout_ms;
    eth_link_t link;
    bool initialized;
};
static Device *ctx(esp_eth_phy_t *phy) { return __containerof(phy, Device, parent); }
static esp_err_t read_reg(Device *p, uint32_t reg, uint32_t *value) {
    if (!p->mediator || !value) return ESP_ERR_INVALID_STATE;
    *value=0xffffffff;
    esp_err_t e=p->mediator->phy_reg_read(p->mediator,p->addr,reg,value);
    if(e!=ESP_OK) LAN9303_DEBUG("MDIO read addr=%lu reg=%lu failed: %s",(unsigned long)p->addr,(unsigned long)reg,esp_err_to_name(e));
    return e;
}
static esp_err_t write_reg(Device *p, uint32_t reg, uint32_t value) {
    if(!p->mediator) return ESP_ERR_INVALID_STATE;
    esp_err_t e=p->mediator->phy_reg_write(p->mediator,p->addr,reg,value);
    if(e!=ESP_OK) LAN9303_DEBUG("MDIO write addr=%lu reg=%lu failed: %s",(unsigned long)p->addr,(unsigned long)reg,esp_err_to_name(e));
    return e;
}
static esp_err_t configure_leds(Device *p)
{
    // LAN9303 system CSR LED_CFG = 0x01BC.
    // SMI mapping: PHY address 22, registers 30 (low) and 31 (high).
    // Requires LAN9303 SMI management mode.

    constexpr uint32_t SMI_PHY = 22;
    constexpr uint32_t REG_LO  = 30;
    constexpr uint32_t REG_HI  = 31;

    constexpr uint32_t LED_CFG = 0x0000023F;
    uint32_t lo = 0, hi = 0;

    esp_err_t e = p->mediator->phy_reg_read(
        p->mediator, SMI_PHY, REG_LO, &lo);
    if (e != ESP_OK) return e;

    e = p->mediator->phy_reg_read(
        p->mediator, SMI_PHY, REG_HI, &hi);
    if (e != ESP_OK) return e;

    LAN9303_DEBUG( "LED_CFG before: 0x%04lX%04lX",
                (unsigned long)(hi & 0xFFFF),
                (unsigned long)(lo & 0xFFFF));

    // Reject an invalid SMI response.
    if ((lo & 0xFFFF) == 0xFFFF &&
        (hi & 0xFFFF) == 0xFFFF)
        return ESP_ERR_NOT_FOUND;

    // Preserve unrelated register bits.
    uint32_t value = ((hi & 0xFFFF) << 16) | (lo & 0xFFFF);
    value = (value & ~0x0000033F) | LED_CFG;

    e = p->mediator->phy_reg_write(
        p->mediator, SMI_PHY, REG_LO, value & 0xFFFF);
    if (e != ESP_OK) return e;

    e = p->mediator->phy_reg_write(
        p->mediator, SMI_PHY, REG_HI, value >> 16);
    if (e != ESP_OK) return e;

    // Read back LED_CFG after writing
    uint32_t read_lo = 0;
    uint32_t read_hi = 0;

    e = p->mediator->phy_reg_read(
        p->mediator, SMI_PHY, REG_LO, &read_lo);
    if (e != ESP_OK) return e;

    e = p->mediator->phy_reg_read(
        p->mediator, SMI_PHY, REG_HI, &read_hi);
    if (e != ESP_OK) return e;

    uint32_t readback =
        ((read_hi & 0xFFFF) << 16) | (read_lo & 0xFFFF);

    LAN9303_DEBUG( "LED_CFG written:  0x%08lX",
                (unsigned long)value);

    LAN9303_DEBUG( "LED_CFG readback: 0x%08lX",
                (unsigned long)readback);
    if (readback != value) {
        LAN9303_DEBUG( "LED_CFG VERIFY FAILED!");
        return ESP_ERR_INVALID_RESPONSE;
    }

    LAN9303_DEBUG( "LED_CFG VERIFY OK");

    return ESP_OK;
}
static esp_err_t enable_auto_mdix(Device *p)
{
    for (uint32_t port = 1; port <= 2; port++) {
        uint32_t value = 0;

        esp_err_t err = p->mediator->phy_reg_read(
            p->mediator, port, 27, &value);

        if (err != ESP_OK) return err;

        // Enable software override and Auto-MDIX.
        uint32_t new_value = value | 0xC000;

        err = p->mediator->phy_reg_write(
            p->mediator, port, 27, new_value);

        if (err != ESP_OK) return err;

        uint32_t verify = 0;
        err = p->mediator->phy_reg_read(
            p->mediator, port, 27, &verify);

        if (err != ESP_OK) return err;

        if ((verify & 0xC000) != 0xC000)
            return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}
static esp_err_t set_mediator(esp_eth_phy_t *phy,esp_eth_mediator_t *m) {
    if(!m) return ESP_ERR_INVALID_ARG;
    ctx(phy)->mediator=m;
    LAN9303_DEBUG("mediator installed");
    return ESP_OK;
}
// Strict, non-destructive Virtual PHY verification. The ID registers are programmable
// and legitimately default to 0x0000; the hardware mode bits are more useful.
static esp_err_t verify(Device *p) {
    uint32_t ctrl,stat,id1,id2,adv,partner,special;
    esp_err_t e;
#define READ(r,v) do { e=read_reg(p,(r),&(v)); if(e!=ESP_OK) return e; } while(0)
    READ(BMCR,ctrl); READ(BMSR,stat); READ(ID1,id1); READ(ID2,id2);
    READ(ANAR,adv); READ(ANLPAR,partner); READ(SPECIAL,special);
#undef READ
    LAN9303_DEBUG("VPHY@%lu BMCR=%04lx BMSR=%04lx ID=%04lx:%04lx ANAR=%04lx ANLPAR=%04lx SPECIAL=%04lx",
        (unsigned long)p->addr,(unsigned long)(ctrl&0xffff),(unsigned long)(stat&0xffff),
        (unsigned long)(id1&0xffff),(unsigned long)(id2&0xffff),
        (unsigned long)(adv&0xffff),(unsigned long)(partner&0xffff),(unsigned long)(special&0xffff));
    if (ctrl==0xffff && stat==0xffff && special==0xffff) {
        LAN9303_DEBUG("MDIO bus returned all ones; no verified device"); return ESP_ERR_NOT_FOUND;
    }
    // LAN9303 Virtual PHY supports 10/100 full/half duplex, bits 14..11.
    if ((stat & 0x7800u)!=0x7800u) {
        LAN9303_DEBUG("Invalid VPHY capabilities: BMSR=0x%04lx",(unsigned long)stat); return ESP_ERR_INVALID_RESPONSE;
    }
    if ((special & SPECIAL_MODE_MASK)!=SPECIAL_RMII_PHY) {
        LAN9303_DEBUG("Port0 not RMII PHY mode: SPECIAL=0x%04lx",(unsigned long)special); return ESP_ERR_INVALID_STATE;
    }
    if (!(special & SPECIAL_CLOCK_OUT)) {
        LAN9303_DEBUG("Port0 RMII 50MHz clock is not configured as OUTPUT"); return ESP_ERR_INVALID_STATE;
    }
    if (special & SPECIAL_TURBO) {
        LAN9303_DEBUG("Unexpected Turbo MII mode"); return ESP_ERR_INVALID_STATE;
    }
    if ((adv & 0x001fu)!=ANAR_SELECTOR) {
        LAN9303_DEBUG("Invalid Virtual PHY ANAR selector: 0x%04lx",(unsigned long)adv); return ESP_ERR_INVALID_RESPONSE;
    }
    if (!(stat & BMSR_LINK)) {
        LAN9303_DEBUG("Virtual PHY internal link not up"); return ESP_ERR_INVALID_STATE;
    }
    LAN9303_DEBUG("LAN9303 Virtual PHY verified (RMII PHY, clock out)");
    return ESP_OK;
}
static esp_err_t init(esp_eth_phy_t *phy) {
    Device *p=ctx(phy);
    LAN9303_DEBUG("init entered; MDIO address=%lu",(unsigned long)p->addr);
    if(!p->mediator) return ESP_ERR_INVALID_STATE;
    // LC3208 uses the strapped Virtual PHY address 0. Never auto-detect a cable PHY.
    if(p->addr!=0) { LAN9303_DEBUG("LC3208 requires VPHY address 0, got %lu",(unsigned long)p->addr); return ESP_ERR_INVALID_ARG; }
    p->initialized=false;
    esp_err_t e=verify(p);
    if(e!=ESP_OK) { LAN9303_DEBUG("LAN9303 initialization FAILED: %s",esp_err_to_name(e)); return e; }
    e = enable_auto_mdix(p);
    if (e != ESP_OK) return e;
    /*
    LED_CFG = 0x0000003F by default, 
    Port 1 Link/Activity : LED2(pin27), Speed : LED0(pin29)
    Port 2 Link/Activity : LED5(pin24), Speed : LED3(pin26)

    esp_err_t led_err = configure_leds(p);
    if (led_err != ESP_OK) {
        LAN9303_DEBUG( "LAN9303 LED configuration failed: %s",
                esp_err_to_name(led_err));
    }  
    */  
    p->link=ETH_LINK_DOWN;
    p->initialized=true;
    return ESP_OK;
}
static esp_err_t get_link(esp_eth_phy_t *phy) {
    Device *p=ctx(phy);
    if(!p->initialized) return ESP_ERR_INVALID_STATE;
    uint32_t stat,special;
    esp_err_t e=read_reg(p,BMSR,&stat); if(e!=ESP_OK) return e;
    e=read_reg(p,BMSR,&stat); if(e!=ESP_OK) return e;
    e=read_reg(p,SPECIAL,&special); if(e!=ESP_OK) return e;
    if((special & SPECIAL_MODE_MASK)!=SPECIAL_RMII_PHY) return ESP_ERR_INVALID_STATE;
    eth_link_t link=(stat & BMSR_LINK)?ETH_LINK_UP:ETH_LINK_DOWN;
    if(link==p->link) return ESP_OK;
    if(link==ETH_LINK_UP) {
        // SPECIAL[4:2]: 001=10H,010=100H,101=10F,110=100F.
        uint32_t mode=(special & SPECIAL_SPEED_DUPLEX_MASK)>>2;
        if(mode!=1 && mode!=2 && mode!=5 && mode!=6) {
            LAN9303_DEBUG("VPHY speed/duplex not settled: SPECIAL=0x%04lx",(unsigned long)special);
            return ESP_OK; // retry at next poll, do not falsely announce Link
        }
        eth_speed_t speed=(mode==2 || mode==6)?ETH_SPEED_100M:ETH_SPEED_10M;
        eth_duplex_t duplex=(mode==5 || mode==6)?ETH_DUPLEX_FULL:ETH_DUPLEX_HALF;
        e=p->mediator->on_state_changed(p->mediator,ETH_STATE_SPEED,(void *)(uintptr_t)speed);
        if(e!=ESP_OK) return e;
        e=p->mediator->on_state_changed(p->mediator,ETH_STATE_DUPLEX,(void *)(uintptr_t)duplex);
        if(e!=ESP_OK) return e;
        LAN9303_DEBUG("CPU Port0 Virtual PHY UP: %s %s",speed==ETH_SPEED_100M?"100M":"10M",duplex==ETH_DUPLEX_FULL?"full":"half");
    } else LAN9303_DEBUG("CPU Port0 Virtual PHY DOWN");
    e=p->mediator->on_state_changed(p->mediator,ETH_STATE_LINK,(void *)(uintptr_t)link);
    if(e==ESP_OK) p->link=link;
    return e;
}
static esp_err_t reset(esp_eth_phy_t *phy) {
    Device *p=ctx(phy);
    if(!p->mediator) return ESP_ERR_INVALID_STATE;
    uint32_t ctrl;
    esp_err_t e=read_reg(p,BMCR,&ctrl); if(e!=ESP_OK) return e;
    e=write_reg(p,BMCR,ctrl|RESET); if(e!=ESP_OK) return e;
    uint32_t count=p->reset_timeout_ms/10+1;
    for(uint32_t i=0;i<count;i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        e=read_reg(p,BMCR,&ctrl); if(e!=ESP_OK) return e;
        if(!(ctrl&RESET)) return verify(p);
    }
    return ESP_ERR_TIMEOUT;
}
static esp_err_t reset_hw(esp_eth_phy_t *) {
    // GPIO33 reset and GPIO16 oscillator sequence are managed by ETH.cpp.
    return ESP_OK;
}
static esp_err_t negotiate(esp_eth_phy_t *phy) {
    Device *p=ctx(phy);
    uint32_t ctrl;
    esp_err_t e=read_reg(p,BMCR,&ctrl); if(e!=ESP_OK) return e;
    e=write_reg(p,BMCR,ctrl|AN_ENABLE|AN_RESTART); if(e!=ESP_OK) return e;
    uint32_t stat=0;
    for(uint32_t i=0;i<p->autonego_timeout_ms/10+1;i++) {
        e=read_reg(p,BMSR,&stat); if(e!=ESP_OK) return e;
        if(stat&BMSR_AN_COMPLETE) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}
static esp_err_t pwrctl(esp_eth_phy_t *phy,bool enable) {
    // Virtual PHY power-down bit is documented as having no effect.
    // Do not power down the LAN9303 switch through this callback.
    (void)phy; (void)enable;
    return ESP_OK;
}
static esp_err_t set_addr(esp_eth_phy_t *phy,uint32_t addr) {
    if(addr!=0) return ESP_ERR_INVALID_ARG;
    ctx(phy)->addr=addr; return ESP_OK;
}
static esp_err_t get_addr(esp_eth_phy_t *phy,uint32_t *addr) {
    if(!addr) return ESP_ERR_INVALID_ARG;
    *addr=ctx(phy)->addr; return ESP_OK;
}
static esp_err_t loopback(esp_eth_phy_t *phy,bool enable) {
    Device *p=ctx(phy); uint32_t ctrl;
    esp_err_t e=read_reg(p,BMCR,&ctrl); if(e!=ESP_OK) return e;
    return write_reg(p,BMCR,enable?(ctrl|LOOPBACK):(ctrl&~LOOPBACK));
}
static esp_err_t advertise_pause(esp_eth_phy_t *phy,uint32_t ability) {
    Device *p=ctx(phy); uint32_t adv;
    esp_err_t e=read_reg(p,ANAR,&adv); if(e!=ESP_OK) return e;
    // ESP-IDF's ability is a boolean: advertise symmetric pause when nonzero.
    adv=ability?(adv|0x0400u):(adv&~0x0400u);
    return write_reg(p,ANAR,adv);
}
static esp_err_t deinit(esp_eth_phy_t *phy) {
    Device *p=ctx(phy); p->initialized=false; p->link=ETH_LINK_DOWN; return ESP_OK;
}
static esp_err_t del(esp_eth_phy_t *phy) { free(ctx(phy)); return ESP_OK; }
} // namespace
extern "C" esp_eth_phy_t *esp_eth_phy_new_lan9303(const eth_phy_config_t *config) {

    LAN9303_DEBUG( "LAN9303 constructor entered");
    
    if (config == nullptr) {
        LAN9303_DEBUG( "PHY config is NULL");
        return nullptr;
    }

    LAN9303_DEBUG(
            "PHY config: addr=%d reset_gpio=%d "
            "reset_timeout=%u autonego_timeout=%u",
            config->phy_addr,
            config->reset_gpio_num,
            (unsigned)config->reset_timeout_ms,
            (unsigned)config->autonego_timeout_ms);


    Device *p=(Device *)calloc(1,sizeof(Device)); if(!p) return nullptr;
    p->addr=0; p->reset_timeout_ms=config->reset_timeout_ms?config->reset_timeout_ms:1000;
    p->autonego_timeout_ms=config->autonego_timeout_ms?config->autonego_timeout_ms:4000;
    p->link=ETH_LINK_DOWN;
    p->parent.set_mediator=set_mediator;
    p->parent.init=init; p->parent.deinit=deinit;
    p->parent.reset=reset; p->parent.reset_hw=reset_hw;
    p->parent.negotiate=negotiate; p->parent.pwrctl=pwrctl;
    p->parent.get_link=get_link; p->parent.set_addr=set_addr; p->parent.get_addr=get_addr;
    p->parent.loopback=loopback; p->parent.advertise_pause_ability=advertise_pause;
    p->parent.del=del;
    return &p->parent;
}
