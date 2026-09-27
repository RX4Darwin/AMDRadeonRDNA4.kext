/** @file
  GOP driver for the emulated RX 9070 XT (QEMU `rdna4` device).

  The device powers on with its display pipe already lit, the way the real
  card's GOP leaves it: OTG0 at 1920x1080, HUBP0 scanning VRAM offset 0 with
  a 1920-pixel pitch. This driver only describes that framebuffer (BAR0) to
  the firmware; it never programs the device.

  Derived from UefiPayloadPkg/GraphicsOutputDxe/GraphicsOutput.c.
  Copyright (c) 2016, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <IndustryStandard/Pci.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>

#define RDNA_VENDOR_ID  0x1002
#define RDNA_DEVICE_ID  0x7550
#define RDNA_FB_BAR     0
#define RDNA_WIDTH      1920
#define RDNA_HEIGHT     1080

typedef struct {
  UINT32                                  Signature;
  EFI_HANDLE                              GraphicsOutputHandle;
  EFI_GRAPHICS_OUTPUT_PROTOCOL            GraphicsOutput;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       GraphicsOutputMode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION    ModeInfo;
  EFI_DEVICE_PATH_PROTOCOL                *DevicePath;
  EFI_PCI_IO_PROTOCOL                     *PciIo;
  UINT64                                  PciAttributes;
  FRAME_BUFFER_CONFIGURE                  *BltConfigure;
  UINTN                                   BltConfigureSize;
} RDNA_GOP_PRIVATE;

#define RDNA_GOP_SIGNATURE  SIGNATURE_32 ('r', 'g', 'o', 'p')
#define RDNA_GOP_FROM_THIS(a) \
  CR(a, RDNA_GOP_PRIVATE, GraphicsOutput, RDNA_GOP_SIGNATURE)

STATIC CONST ACPI_ADR_DEVICE_PATH  mAdrNode = {
  {
    ACPI_DEVICE_PATH,
    ACPI_ADR_DP,
    { sizeof (ACPI_ADR_DEVICE_PATH), 0 },
  },
  ACPI_DISPLAY_ADR (1, 0, 0, 1, 0, ACPI_ADR_DISPLAY_TYPE_EXTERNAL_DIGITAL, 0, 0)
};

STATIC
EFI_STATUS
EFIAPI
RdnaGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((This == NULL) || (Info == NULL) || (SizeOfInfo == NULL) || (ModeNumber >= This->Mode->MaxMode)) {
    return EFI_INVALID_PARAMETER;
  }

  *SizeOfInfo = This->Mode->SizeOfInfo;
  *Info       = AllocateCopyPool (*SizeOfInfo, This->Mode->Info);
  return (*Info == NULL) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RdnaGopSetMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN  UINT32                        ModeNumber
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;
  RDNA_GOP_PRIVATE               *Private;

  if (ModeNumber >= This->Mode->MaxMode) {
    return EFI_UNSUPPORTED;
  }

  Private = RDNA_GOP_FROM_THIS (This);
  ZeroMem (&Black, sizeof (Black));
  return RETURN_ERROR (
           FrameBufferBlt (
             Private->BltConfigure,
             &Black,
             EfiBltVideoFill,
             0,
             0,
             0,
             0,
             This->Mode->Info->HorizontalResolution,
             This->Mode->Info->VerticalResolution,
             0
             )
           ) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RdnaGopBlt (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer  OPTIONAL,
  IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN  UINTN                              SourceX,
  IN  UINTN                              SourceY,
  IN  UINTN                              DestinationX,
  IN  UINTN                              DestinationY,
  IN  UINTN                              Width,
  IN  UINTN                              Height,
  IN  UINTN                              Delta         OPTIONAL
  )
{
  RETURN_STATUS     Status;
  EFI_TPL           Tpl;
  RDNA_GOP_PRIVATE  *Private;

  Private = RDNA_GOP_FROM_THIS (This);
  Tpl     = gBS->RaiseTPL (TPL_NOTIFY);
  Status  = FrameBufferBlt (
              Private->BltConfigure,
              BltBuffer,
              BltOperation,
              SourceX,
              SourceY,
              DestinationX,
              DestinationY,
              Width,
              Height,
              Delta
              );
  gBS->RestoreTPL (Tpl);
  return RETURN_ERROR (Status) ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

/** The emulated card: AMD 1002:7550, display class. */
STATIC
BOOLEAN
IsRdnaDevice (
  IN EFI_PCI_IO_PROTOCOL  *PciIo
  )
{
  PCI_TYPE00  Pci;

  if (EFI_ERROR (PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, 0, sizeof (Pci), &Pci))) {
    return FALSE;
  }

  return IS_PCI_DISPLAY (&Pci) &&
         (Pci.Hdr.VendorId == RDNA_VENDOR_ID) &&
         (Pci.Hdr.DeviceId == RDNA_DEVICE_ID);
}

STATIC
EFI_STATUS
EFIAPI
RdnaGopSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  BOOLEAN              Match;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Match = IsRdnaDevice (PciIo);
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  if (!Match) {
    return EFI_UNSUPPORTED;
  }

  if ((RemainingDevicePath == NULL) || IsDevicePathEnd (RemainingDevicePath) ||
      (CompareMem (RemainingDevicePath, &mAdrNode, sizeof (mAdrNode)) == 0))
  {
    return EFI_SUCCESS;
  }

  return EFI_INVALID_PARAMETER;
}

STATIC
EFI_STATUS
EFIAPI
RdnaGopStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                         Status;
  RETURN_STATUS                      ReturnStatus;
  RDNA_GOP_PRIVATE                   *Private;
  EFI_PCI_IO_PROTOCOL                *PciIo;
  EFI_DEVICE_PATH_PROTOCOL           *PciDevicePath;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Resources;
  UINT64                             FrameBufferSize;

  Private         = NULL;
  FrameBufferSize = (UINT64)RDNA_WIDTH * RDNA_HEIGHT * 4;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&PciDevicePath,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    goto ClosePciIo;
  }

  Status = PciIo->GetBarAttributes (PciIo, RDNA_FB_BAR, NULL, (VOID **)&Resources);
  if (EFI_ERROR (Status)) {
    goto CloseDevicePath;
  }

  if ((Resources->Desc != ACPI_ADDRESS_SPACE_DESCRIPTOR) ||
      (Resources->ResType != ACPI_ADDRESS_SPACE_TYPE_MEM) ||
      (Resources->AddrLen < FrameBufferSize))
  {
    FreePool (Resources);
    Status = EFI_UNSUPPORTED;
    goto CloseDevicePath;
  }

  if ((RemainingDevicePath != NULL) && IsDevicePathEnd (RemainingDevicePath)) {
    FreePool (Resources);
    return EFI_SUCCESS;
  }

  Private = AllocateZeroPool (sizeof (*Private));
  if (Private == NULL) {
    FreePool (Resources);
    Status = EFI_OUT_OF_RESOURCES;
    goto CloseDevicePath;
  }

  Private->Signature                   = RDNA_GOP_SIGNATURE;
  Private->GraphicsOutput.QueryMode    = RdnaGopQueryMode;
  Private->GraphicsOutput.SetMode      = RdnaGopSetMode;
  Private->GraphicsOutput.Blt          = RdnaGopBlt;
  Private->GraphicsOutput.Mode         = &Private->GraphicsOutputMode;
  Private->ModeInfo.Version            = 0;
  Private->ModeInfo.HorizontalResolution = RDNA_WIDTH;
  Private->ModeInfo.VerticalResolution   = RDNA_HEIGHT;
  Private->ModeInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  Private->ModeInfo.PixelsPerScanLine    = RDNA_WIDTH;
  Private->GraphicsOutputMode.MaxMode    = 1;
  Private->GraphicsOutputMode.Mode       = 0;
  Private->GraphicsOutputMode.Info       = &Private->ModeInfo;
  Private->GraphicsOutputMode.SizeOfInfo = sizeof (Private->ModeInfo);
  Private->GraphicsOutputMode.FrameBufferBase = Resources->AddrRangeMin;
  Private->GraphicsOutputMode.FrameBufferSize = (UINTN)FrameBufferSize;
  FreePool (Resources);

  DEBUG ((
    DEBUG_INFO,
    "RdnaGop: framebuffer %dx%d at 0x%lx\n",
    RDNA_WIDTH,
    RDNA_HEIGHT,
    Private->GraphicsOutputMode.FrameBufferBase
    ));

  Status = PciIo->Attributes (PciIo, EfiPciIoAttributeOperationGet, 0, &Private->PciAttributes);
  if (!EFI_ERROR (Status)) {
    Status = PciIo->Attributes (PciIo, EfiPciIoAttributeOperationEnable, EFI_PCI_DEVICE_ENABLE, NULL);
  }

  if (EFI_ERROR (Status)) {
    goto FreePrivate;
  }

  ReturnStatus = FrameBufferBltConfigure (
                   (VOID *)(UINTN)Private->GraphicsOutputMode.FrameBufferBase,
                   &Private->ModeInfo,
                   Private->BltConfigure,
                   &Private->BltConfigureSize
                   );
  if (ReturnStatus == RETURN_BUFFER_TOO_SMALL) {
    Private->BltConfigure = AllocatePool (Private->BltConfigureSize);
    if (Private->BltConfigure != NULL) {
      ReturnStatus = FrameBufferBltConfigure (
                       (VOID *)(UINTN)Private->GraphicsOutputMode.FrameBufferBase,
                       &Private->ModeInfo,
                       Private->BltConfigure,
                       &Private->BltConfigureSize
                       );
    }
  }

  if (RETURN_ERROR (ReturnStatus)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto RestoreAttributes;
  }

  Private->DevicePath = AppendDevicePathNode (PciDevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&mAdrNode);
  if (Private->DevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto RestoreAttributes;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Private->GraphicsOutputHandle,
                  &gEfiGraphicsOutputProtocolGuid,
                  &Private->GraphicsOutput,
                  &gEfiDevicePathProtocolGuid,
                  Private->DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    goto RestoreAttributes;
  }

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&Private->PciIo,
                  This->DriverBindingHandle,
                  Private->GraphicsOutputHandle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                  );
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  gBS->UninstallMultipleProtocolInterfaces (
         Private->GraphicsOutputHandle,
         &gEfiGraphicsOutputProtocolGuid,
         &Private->GraphicsOutput,
         &gEfiDevicePathProtocolGuid,
         Private->DevicePath,
         NULL
         );

RestoreAttributes:
  PciIo->Attributes (PciIo, EfiPciIoAttributeOperationSet, Private->PciAttributes, NULL);

FreePrivate:
  if (Private->DevicePath != NULL) {
    FreePool (Private->DevicePath);
  }

  if (Private->BltConfigure != NULL) {
    FreePool (Private->BltConfigure);
  }

  FreePool (Private);

CloseDevicePath:
  gBS->CloseProtocol (Controller, &gEfiDevicePathProtocolGuid, This->DriverBindingHandle, Controller);

ClosePciIo:
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RdnaGopStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  RDNA_GOP_PRIVATE              *Private;

  if (NumberOfChildren == 0) {
    gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
    gBS->CloseProtocol (Controller, &gEfiDevicePathProtocolGuid, This->DriverBindingHandle, Controller);
    return EFI_SUCCESS;
  }

  Status = gBS->OpenProtocol (
                  ChildHandleBuffer[0],
                  &gEfiGraphicsOutputProtocolGuid,
                  (VOID **)&Gop,
                  This->DriverBindingHandle,
                  ChildHandleBuffer[0],
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private = RDNA_GOP_FROM_THIS (Gop);
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Private->GraphicsOutputHandle);
  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Private->GraphicsOutputHandle,
                  &gEfiGraphicsOutputProtocolGuid,
                  &Private->GraphicsOutput,
                  &gEfiDevicePathProtocolGuid,
                  Private->DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    gBS->OpenProtocol (
           Controller,
           &gEfiPciIoProtocolGuid,
           (VOID **)&Private->PciIo,
           This->DriverBindingHandle,
           Private->GraphicsOutputHandle,
           EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
           );
    return Status;
  }

  Private->PciIo->Attributes (Private->PciIo, EfiPciIoAttributeOperationSet, Private->PciAttributes, NULL);
  FreePool (Private->DevicePath);
  FreePool (Private->BltConfigure);
  FreePool (Private);
  return EFI_SUCCESS;
}

STATIC EFI_DRIVER_BINDING_PROTOCOL  mRdnaGopDriverBinding = {
  RdnaGopSupported,
  RdnaGopStart,
  RdnaGopStop,
  0x10,
  NULL,
  NULL
};

EFI_STATUS
EFIAPI
RdnaGopEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBinding (ImageHandle, SystemTable, &mRdnaGopDriverBinding, ImageHandle);
}
