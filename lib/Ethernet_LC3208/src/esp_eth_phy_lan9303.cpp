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

static const char *TAG = "lan9303";
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
    if(e!=ESP_OK) ESP_LOGE(TAG,"MDIO read addr=%lu reg=%lu failed: %s",(unsigned long)p->addr,(unsigned long)reg,esp_err_to_name(e));
    return e;
}
static esp_err_t write_reg(Device *p, uint32_t reg, uint32_t value) {
    if(!p->mediator) return ESP_ERR_INVALID_STATE;
    esp_err_t e=p->mediator->phy_reg_write(p->mediator,p->addr,reg,value);
    if(e!=ESP_OK) ESP_LOGE(TAG,"MDIO write addr=%lu reg=%lu failed: %s",(unsigned long)p->addr,(unsigned long)reg,esp_err_to_name(e));
    return e;
}
static esp_err_t set_mediator(esp_eth_phy_t *phy,esp_eth_mediator_t *m) {
    if(!m) return ESP_ERR_INVALID_ARG;
    ctx(phy)->mediator=m;
    ESP_LOGI(TAG,"mediator installed");
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
    ESP_LOGI(TAG,"VPHY@%lu BMCR=%04lx BMSR=%04lx ID=%04lx:%04lx ANAR=%04lx ANLPAR=%04lx SPECIAL=%04lx",
        (unsigned long)p->addr,(unsigned long)(ctrl&0xffff),(unsigned long)(stat&0xffff),
        (unsigned long)(id1&0xffff),(unsigned long)(id2&0xffff),
        (unsigned long)(adv&0xffff),(unsigned long)(partner&0xffff),(unsigned long)(special&0xffff));
    if (ctrl==0xffff && stat==0xffff && special==0xffff) {
        ESP_LOGE(TAG,"MDIO bus returned all ones; no verified device"); return ESP_ERR_NOT_FOUND;
    }
    // LAN9303 Virtual PHY supports 10/100 full/half duplex, bits 14..11.
    if ((stat & 0x7800u)!=0x7800u) {
        ESP_LOGE(TAG,"Invalid VPHY capabilities: BMSR=0x%04lx",(unsigned long)stat); return ESP_ERR_INVALID_RESPONSE;
    }
    if ((special & SPECIAL_MODE_MASK)!=SPECIAL_RMII_PHY) {
        ESP_LOGE(TAG,"Port0 not RMII PHY mode: SPECIAL=0x%04lx",(unsigned long)special); return ESP_ERR_INVALID_STATE;
    }
    if (!(special & SPECIAL_CLOCK_OUT)) {
        ESP_LOGE(TAG,"Port0 RMII 50MHz clock is not configured as OUTPUT"); return ESP_ERR_INVALID_STATE;
    }
    if (special & SPECIAL_TURBO) {
        ESP_LOGE(TAG,"Unexpected Turbo MII mode"); return ESP_ERR_INVALID_STATE;
    }
    if ((adv & 0x001fu)!=ANAR_SELECTOR) {
        ESP_LOGE(TAG,"Invalid Virtual PHY ANAR selector: 0x%04lx",(unsigned long)adv); return ESP_ERR_INVALID_RESPONSE;
    }
    if (!(stat & BMSR_LINK)) {
        ESP_LOGE(TAG,"Virtual PHY internal link not up"); return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG,"LAN9303 Virtual PHY verified (RMII PHY, clock out)");
    return ESP_OK;
}
static esp_err_t init(esp_eth_phy_t *phy) {
    Device *p=ctx(phy);
    ESP_LOGI(TAG,"init entered; MDIO address=%lu",(unsigned long)p->addr);
    if(!p->mediator) return ESP_ERR_INVALID_STATE;
    // LC3208 uses the strapped Virtual PHY address 0. Never auto-detect a cable PHY.
    if(p->addr!=0) { ESP_LOGE(TAG,"LC3208 requires VPHY address 0, got %lu",(unsigned long)p->addr); return ESP_ERR_INVALID_ARG; }
    p->initialized=false;
    esp_err_t e=verify(p);
    if(e!=ESP_OK) { ESP_LOGE(TAG,"LAN9303 initialization FAILED: %s",esp_err_to_name(e)); return e; }
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
            ESP_LOGW(TAG,"VPHY speed/duplex not settled: SPECIAL=0x%04lx",(unsigned long)special);
            return ESP_OK; // retry at next poll, do not falsely announce Link
        }
        eth_speed_t speed=(mode==2 || mode==6)?ETH_SPEED_100M:ETH_SPEED_10M;
        eth_duplex_t duplex=(mode==5 || mode==6)?ETH_DUPLEX_FULL:ETH_DUPLEX_HALF;
        e=p->mediator->on_state_changed(p->mediator,ETH_STATE_SPEED,(void *)(uintptr_t)speed);
        if(e!=ESP_OK) return e;
        e=p->mediator->on_state_changed(p->mediator,ETH_STATE_DUPLEX,(void *)(uintptr_t)duplex);
        if(e!=ESP_OK) return e;
        ESP_LOGI(TAG,"CPU Port0 Virtual PHY UP: %s %s",speed==ETH_SPEED_100M?"100M":"10M",duplex==ETH_DUPLEX_FULL?"full":"half");
    } else ESP_LOGW(TAG,"CPU Port0 Virtual PHY DOWN");
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

    ESP_LOGE("lan9303", "LAN9303 constructor entered");
    
    if (config == nullptr) {
        ESP_LOGE("lan9303", "PHY config is NULL");
        return nullptr;
    }

    ESP_LOGE("lan9303",
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
