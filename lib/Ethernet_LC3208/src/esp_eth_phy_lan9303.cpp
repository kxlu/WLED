//esp_eth_phy_lan9303.cpp
/*
LAN9303's Virtual PHY represents the switch's CPU-facing connection, so its link-status behavior cannot automatically be assumed to match an ordinary LAN8720 PHY. We need to implement that behavior appropriately rather than simply copy the LAN8720 driver.
*/

#include "esp_eth_phy_lan9303.h"

// Temporary implementation for build integration testing.
// This is NOT yet the LAN9303-specific PHY driver.

extern "C" esp_eth_phy_t *esp_eth_phy_new_lan9303(
    const eth_phy_config_t *config
)
{
    if (config == nullptr) {
        return nullptr;
    }

    /*
    we'll replace the temporary implementation with the actual LAN9303 callbacks, starting with MDIO register access and initialization
    */
    return esp_eth_phy_new_lan87xx(config);
}