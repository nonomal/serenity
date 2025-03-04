/*
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibWeb/Bindings/DOMExceptionWrapper.h>
#include <LibWeb/Bindings/IDLAbstractOperations.h>
#include <LibWeb/FileAPI/Blob.h>

namespace Web::FileAPI {

Blob::Blob(ByteBuffer byte_buffer, String type)
    : m_byte_buffer(move(byte_buffer))
    , m_type(move(type))
{
}

// https://w3c.github.io/FileAPI/#ref-for-dom-blob-blob
DOM::ExceptionOr<NonnullRefPtr<Blob>> Blob::create(Optional<Vector<BlobPart>> const& blob_parts, Optional<BlobPropertyBag> const& options)
{
    // 1. If invoked with zero parameters, return a new Blob object consisting of 0 bytes, with size set to 0, and with type set to the empty string.
    if (!blob_parts.has_value() && !options.has_value())
        return adopt_ref(*new Blob());

    ByteBuffer byte_buffer {};
    // 2. Let bytes be the result of processing blob parts given blobParts and options.
    if (blob_parts.has_value()) {
        byte_buffer = TRY_OR_RETURN_OOM(process_blob_parts(blob_parts.value()));
    }

    String type = String::empty();
    // 3. If the type member of the options argument is not the empty string, run the following sub-steps:
    if (options.has_value() && !options->type.is_empty()) {
        // FIXME: 1. Let t be the type dictionary member. If t contains any characters outside the range U+0020 to U+007E, then set t to the empty string and return from these substeps.

        // 2. Convert every character in t to ASCII lowercase.
        type = options->type.to_lowercase();
    }

    // 4. Return a Blob object referring to bytes as its associated byte sequence, with its size set to the length of bytes, and its type set to the value of t from the substeps above.
    return adopt_ref(*new Blob(move(byte_buffer), move(type)));
}

DOM::ExceptionOr<NonnullRefPtr<Blob>> Blob::create_with_global_object(Bindings::WindowObject&, Optional<Vector<BlobPart>> const& blob_parts, Optional<BlobPropertyBag> const& options)
{
    return Blob::create(blob_parts, options);
}

// https://w3c.github.io/FileAPI/#process-blob-parts
ErrorOr<ByteBuffer> Blob::process_blob_parts(Vector<BlobPart> const& blob_parts)
{
    // 1. Let bytes be an empty sequence of bytes.
    ByteBuffer bytes {};

    // 2. For each element in parts:
    for (auto const& blob_part : blob_parts) {
        TRY(blob_part.visit(
            // 1. If element is a USVString, run the following sub-steps:
            [&](String const& string) -> ErrorOr<void> {
                // NOTE: This step is handled by the lambda expression.
                // 1. Let s be element.

                // FIXME: 2. If the endings member of options is "native", set s to the result of converting line endings to native of element.

                // NOTE: The AK::String is always UTF-8.
                // 3. Append the result of UTF-8 encoding s to bytes.
                return bytes.try_append(string.to_byte_buffer());
            },
            // 2. If element is a BufferSource, get a copy of the bytes held by the buffer source, and append those bytes to bytes.
            [&](JS::Handle<JS::Object> const& buffer_source) -> ErrorOr<void> {
                auto data_buffer = Bindings::IDL::get_buffer_source_copy(*buffer_source.cell());
                if (data_buffer.has_value())
                    return bytes.try_append(data_buffer->bytes());
                return {};
            },
            // 3. If element is a Blob, append the bytes it represents to bytes.
            [&](NonnullRefPtr<Blob> const& blob) -> ErrorOr<void> {
                return bytes.try_append(blob->m_byte_buffer.bytes());
            }));
    }
    return bytes;
}

// https://w3c.github.io/FileAPI/#dfn-slice
DOM::ExceptionOr<NonnullRefPtr<Blob>> Blob::slice(Optional<i64> start, Optional<i64> end, Optional<String> const& content_type)
{
    // 1. The optional start parameter is a value for the start point of a slice() call, and must be treated as a byte-order position, with the zeroth position representing the first byte.
    //    User agents must process slice() with start normalized according to the following:
    i64 relative_start;
    if (!start.has_value()) {
        // a. If the optional start parameter is not used as a parameter when making this call, let relativeStart be 0.
        relative_start = 0;
    } else {
        auto start_value = start.value();
        // b. If start is negative, let relativeStart be max((size + start), 0).
        if (start_value < 0) {
            relative_start = max((size() + start_value), 0);
        }
        // c. Else, let relativeStart be min(start, size).
        else {
            relative_start = min(start_value, size());
        }
    }

    // 2. The optional end parameter is a value for the end point of a slice() call. User agents must process slice() with end normalized according to the following:
    i64 relative_end;
    if (!end.has_value()) {
        // a. If the optional end parameter is not used as a parameter when making this call, let relativeEnd be size.
        relative_end = size();
    } else {
        auto end_value = end.value();
        // b. If end is negative, let relativeEnd be max((size + end), 0).
        if (end_value < 0) {
            relative_end = max((size() + end_value), 0);
        }
        // c Else, let relativeEnd be min(end, size).
        else {
            relative_end = min(end_value, size());
        }
    }

    // 3. The optional contentType parameter is used to set the ASCII-encoded string in lower case representing the media type of the Blob.
    //    User agents must process the slice() with contentType normalized according to the following:
    String relative_content_type;
    if (!content_type.has_value()) {
        // a. If the contentType parameter is not provided, let relativeContentType be set to the empty string.
        relative_content_type = "";
    } else {
        // b. Else let relativeContentType be set to contentType and run the substeps below:

        // FIXME: 1. If relativeContentType contains any characters outside the range of U+0020 to U+007E, then set relativeContentType to the empty string and return from these substeps.

        // 2. Convert every character in relativeContentType to ASCII lowercase.
        relative_content_type = content_type->to_lowercase();
    }

    // 4. Let span be max((relativeEnd - relativeStart), 0).
    auto span = max((relative_end - relative_start), 0);

    // 5. Return a new Blob object S with the following characteristics:
    // a. S refers to span consecutive bytes from this, beginning with the byte at byte-order position relativeStart.
    // b. S.size = span.
    // c. S.type = relativeContentType.
    auto byte_buffer = TRY_OR_RETURN_OOM(m_byte_buffer.slice(relative_start, span));
    return adopt_ref(*new Blob(move(byte_buffer), move(relative_content_type)));
}

// https://w3c.github.io/FileAPI/#dom-blob-text
JS::Promise* Blob::text()
{
    auto& global_object = wrapper()->global_object();

    // FIXME: 1. Let stream be the result of calling get stream on this.
    // FIXME: 2. Let reader be the result of getting a reader from stream. If that threw an exception, return a new promise rejected with that exception.

    // FIXME: We still need to implement ReadableStream for this step to be fully valid.
    // 3. Let promise be the result of reading all bytes from stream with reader
    auto* promise = JS::Promise::create(global_object);
    auto* result = JS::js_string(global_object.heap(), String { m_byte_buffer.bytes() });

    // 4. Return the result of transforming promise by a fulfillment handler that returns the result of running UTF-8 decode on its first argument.
    promise->fulfill(result);
    return promise;
}

// https://w3c.github.io/FileAPI/#dom-blob-arraybuffer
JS::Promise* Blob::array_buffer()
{
    auto& global_object = wrapper()->global_object();

    // FIXME: 1. Let stream be the result of calling get stream on this.
    // FIXME: 2. Let reader be the result of getting a reader from stream. If that threw an exception, return a new promise rejected with that exception.

    // FIXME: We still need to implement ReadableStream for this step to be fully valid.
    // 3. Let promise be the result of reading all bytes from stream with reader.
    auto* promise = JS::Promise::create(global_object);
    auto buffer_result = JS::ArrayBuffer::create(global_object, m_byte_buffer.size());
    if (buffer_result.is_error()) {
        promise->reject(buffer_result.release_error().value().release_value());
        return promise;
    }
    auto* buffer = buffer_result.release_value();
    buffer->buffer().overwrite(0, m_byte_buffer.data(), m_byte_buffer.size());

    // 4. Return the result of transforming promise by a fulfillment handler that returns a new ArrayBuffer whose contents are its first argument.
    promise->fulfill(buffer);
    return promise;
}

JS::Object* Blob::create_wrapper(JS::GlobalObject& global_object)
{
    return wrap(global_object, *this);
}

}
