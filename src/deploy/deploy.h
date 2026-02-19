/*
 * deploy.h — Non-destructive deployment to internal ESP
 */

#ifndef SUPERBOOT_DEPLOY_H
#define SUPERBOOT_DEPLOY_H

#include "../superboot.h"

/*
 * Installation paths on the target ESP.
 */
#define SB_DEPLOY_DIR    L"\\EFI\\superboot"
#define SB_DEPLOY_BINARY L"\\EFI\\superboot\\superboot.efi"
#define SB_DEPLOY_LABEL  L"SuperBoot"

#endif /* SUPERBOOT_DEPLOY_H */
