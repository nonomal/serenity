/*
 * Copyright (c) 2020, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <AK/StringView.h>
#include <Kernel/Debug.h>
#include <Kernel/Devices/DeviceManagement.h>
#include <Kernel/FileSystem/OpenFileDescription.h>
#include <Kernel/FileSystem/SysFS/Subsystems/DeviceIdentifiers/BlockDevicesDirectory.h>
#include <Kernel/FileSystem/SysFS/Subsystems/DeviceIdentifiers/SymbolicLinkDeviceComponent.h>
#include <Kernel/FileSystem/SysFS/Subsystems/Devices/Storage/DeviceDirectory.h>
#include <Kernel/FileSystem/SysFS/Subsystems/Devices/Storage/Directory.h>
#include <Kernel/Storage/StorageDevice.h>
#include <Kernel/Storage/StorageManagement.h>
#include <LibC/sys/ioctl_numbers.h>

namespace Kernel {

StorageDevice::StorageDevice(LUNAddress logical_unit_number_address, MajorNumber major, MinorNumber minor, size_t sector_size, u64 max_addressable_block, NonnullOwnPtr<KString> device_name)
    : BlockDevice(major, minor, sector_size)
    , m_early_storage_device_name(move(device_name))
    , m_logical_unit_number_address(logical_unit_number_address)
    , m_max_addressable_block(max_addressable_block)
    , m_blocks_per_page(PAGE_SIZE / block_size())
{
}

void StorageDevice::after_inserting()
{
    after_inserting_add_to_device_management();
    auto sysfs_storage_device_directory = StorageDeviceSysFSDirectory::create(SysFSStorageDirectory::the(), *this);
    m_sysfs_device_directory = sysfs_storage_device_directory;
    SysFSStorageDirectory::the().plug({}, *sysfs_storage_device_directory);
    VERIFY(!m_symlink_sysfs_component);
    auto sys_fs_component = MUST(SysFSSymbolicLinkDeviceComponent::try_create(SysFSDeviceIdentifiersDirectory::the(), *this, *m_sysfs_device_directory));
    m_symlink_sysfs_component = sys_fs_component;
    after_inserting_add_symlink_to_device_identifier_directory();
}

void StorageDevice::will_be_destroyed()
{
    VERIFY(m_symlink_sysfs_component);
    before_will_be_destroyed_remove_symlink_from_device_identifier_directory();
    m_symlink_sysfs_component.clear();
    SysFSStorageDirectory::the().unplug({}, *m_sysfs_device_directory);
    before_will_be_destroyed_remove_from_device_management();
}

StringView StorageDevice::class_name() const
{
    return "StorageDevice"sv;
}

StringView StorageDevice::command_set_to_string_view() const
{
    switch (command_set()) {
    case CommandSet::PlainMemory:
        return "memory"sv;
    case CommandSet::SCSI:
        return "scsi"sv;
    case CommandSet::ATA:
        return "ata"sv;
    case CommandSet::NVMe:
        return "nvme"sv;
    default:
        break;
    }
    VERIFY_NOT_REACHED();
}

StringView StorageDevice::interface_type_to_string_view() const
{
    switch (interface_type()) {
    case InterfaceType::PlainMemory:
        return "memory"sv;
    case InterfaceType::SCSI:
        return "scsi"sv;
    case InterfaceType::ATA:
        return "ata"sv;
    case InterfaceType::NVMe:
        return "nvme"sv;
    default:
        break;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<size_t> StorageDevice::read(OpenFileDescription&, u64 offset, UserOrKernelBuffer& outbuf, size_t len)
{
    u64 index = offset >> block_size_log();
    off_t offset_within_block = 0;
    size_t whole_blocks = len >> block_size_log();
    size_t remaining = len - (whole_blocks << block_size_log());

    // PATAChannel will chuck a wobbly if we try to read more than PAGE_SIZE
    // at a time, because it uses a single page for its DMA buffer.
    if (whole_blocks >= m_blocks_per_page) {
        whole_blocks = m_blocks_per_page;
        remaining = 0;
    }

    if (len < block_size())
        offset_within_block = offset - (index << block_size_log());

    dbgln_if(STORAGE_DEVICE_DEBUG, "StorageDevice::read() index={}, whole_blocks={}, remaining={}", index, whole_blocks, remaining);

    if (whole_blocks > 0) {
        auto read_request = TRY(try_make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Read, index, whole_blocks, outbuf, whole_blocks * block_size()));
        auto result = read_request->wait();
        if (result.wait_result().was_interrupted())
            return EINTR;
        switch (result.request_result()) {
        case AsyncDeviceRequest::Failure:
        case AsyncDeviceRequest::Cancelled:
            return EIO;
        case AsyncDeviceRequest::MemoryFault:
            return EFAULT;
        default:
            break;
        }
    }

    off_t pos = whole_blocks * block_size();

    if (remaining > 0) {
        auto data = TRY(ByteBuffer::create_uninitialized(block_size()));
        auto data_buffer = UserOrKernelBuffer::for_kernel_buffer(data.data());
        auto read_request = TRY(try_make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Read, index + whole_blocks, 1, data_buffer, block_size()));
        auto result = read_request->wait();
        if (result.wait_result().was_interrupted())
            return EINTR;
        switch (result.request_result()) {
        case AsyncDeviceRequest::Failure:
            return pos;
        case AsyncDeviceRequest::Cancelled:
            return EIO;
        case AsyncDeviceRequest::MemoryFault:
            // This should never happen, we're writing to a kernel buffer!
            VERIFY_NOT_REACHED();
        default:
            break;
        }
        TRY(outbuf.write(data.offset_pointer(offset_within_block), pos, remaining));
    }

    return pos + remaining;
}

bool StorageDevice::can_read(OpenFileDescription const&, u64 offset) const
{
    return offset < (max_addressable_block() * block_size());
}

ErrorOr<size_t> StorageDevice::write(OpenFileDescription&, u64 offset, UserOrKernelBuffer const& inbuf, size_t len)
{
    u64 index = offset >> block_size_log();
    off_t offset_within_block = 0;
    size_t whole_blocks = len >> block_size_log();
    size_t remaining = len - (whole_blocks << block_size_log());

    // PATAChannel will chuck a wobbly if we try to write more than PAGE_SIZE
    // at a time, because it uses a single page for its DMA buffer.
    if (whole_blocks >= m_blocks_per_page) {
        whole_blocks = m_blocks_per_page;
        remaining = 0;
    }

    if (len < block_size())
        offset_within_block = offset - (index << block_size_log());

    // We try to allocate the temporary block buffer for partial writes *before* we start any full block writes,
    // to try and prevent partial writes
    Optional<ByteBuffer> partial_write_block;
    if (remaining > 0)
        partial_write_block = TRY(ByteBuffer::create_zeroed(block_size()));

    dbgln_if(STORAGE_DEVICE_DEBUG, "StorageDevice::write() index={}, whole_blocks={}, remaining={}", index, whole_blocks, remaining);

    if (whole_blocks > 0) {
        auto write_request = TRY(try_make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Write, index, whole_blocks, inbuf, whole_blocks * block_size()));
        auto result = write_request->wait();
        if (result.wait_result().was_interrupted())
            return EINTR;
        switch (result.request_result()) {
        case AsyncDeviceRequest::Failure:
        case AsyncDeviceRequest::Cancelled:
            return EIO;
        case AsyncDeviceRequest::MemoryFault:
            return EFAULT;
        default:
            break;
        }
    }

    off_t pos = whole_blocks * block_size();

    // since we can only write in block_size() increments, if we want to do a
    // partial write, we have to read the block's content first, modify it,
    // then write the whole block back to the disk.
    if (remaining > 0) {
        auto data_buffer = UserOrKernelBuffer::for_kernel_buffer(partial_write_block->data());
        {
            auto read_request = TRY(try_make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Read, index + whole_blocks, 1, data_buffer, block_size()));
            auto result = read_request->wait();
            if (result.wait_result().was_interrupted())
                return EINTR;
            switch (result.request_result()) {
            case AsyncDeviceRequest::Failure:
                return pos;
            case AsyncDeviceRequest::Cancelled:
                return EIO;
            case AsyncDeviceRequest::MemoryFault:
                // This should never happen, we're writing to a kernel buffer!
                VERIFY_NOT_REACHED();
            default:
                break;
            }
        }

        TRY(inbuf.read(partial_write_block->offset_pointer(offset_within_block), pos, remaining));

        {
            auto write_request = TRY(try_make_request<AsyncBlockDeviceRequest>(AsyncBlockDeviceRequest::Write, index + whole_blocks, 1, data_buffer, block_size()));
            auto result = write_request->wait();
            if (result.wait_result().was_interrupted())
                return EINTR;
            switch (result.request_result()) {
            case AsyncDeviceRequest::Failure:
                return pos;
            case AsyncDeviceRequest::Cancelled:
                return EIO;
            case AsyncDeviceRequest::MemoryFault:
                // This should never happen, we're writing to a kernel buffer!
                VERIFY_NOT_REACHED();
            default:
                break;
            }
        }
    }

    return pos + remaining;
}

StringView StorageDevice::early_storage_name() const
{
    return m_early_storage_device_name->view();
}

bool StorageDevice::can_write(OpenFileDescription const&, u64 offset) const
{
    return offset < (max_addressable_block() * block_size());
}

ErrorOr<void> StorageDevice::ioctl(OpenFileDescription&, unsigned request, Userspace<void*> arg)
{
    switch (request) {
    case STORAGE_DEVICE_GET_SIZE: {
        u64 disk_size = m_max_addressable_block * block_size();
        return copy_to_user(static_ptr_cast<u64*>(arg), &disk_size);
        break;
    }
    case STORAGE_DEVICE_GET_BLOCK_SIZE: {
        size_t size = block_size();
        return copy_to_user(static_ptr_cast<size_t*>(arg), &size);
        break;
    }
    default:
        return EINVAL;
    }
}

}
