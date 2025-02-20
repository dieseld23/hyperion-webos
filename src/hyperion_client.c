#include "hyperion_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "hyperion_request_builder.h"
#include "hyperion_reply_reader.h"

static int _send_message(const void *buffer, size_t size);
static bool _parse_reply(hyperionnet_Reply_table_t reply);

static int sockfd;
static bool _registered = false;
static int _priority = 0;
static const char *_origin = NULL;
static bool _connected = false;
unsigned char recvBuff[1024];

int hyperion_client(const char *origin, const char *hostname, int port, int priority)
{
    _origin = origin;
    _priority = priority;
    _connected = false;
    _registered = false;
    sockfd = 0;
    struct sockaddr_in serv_addr;
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        fprintf(stderr, "Error : Could not create socket \n");
        return 1;
    }
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout,
                   sizeof(timeout)) < 0)
    {
        fprintf(stderr, "setsockopt failed\n");
        return 1;
    }

    memset(&serv_addr, '0', sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, hostname, &serv_addr.sin_addr) <= 0)
    {
        printf("\n inet_pton error occured\n");
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        printf("\n Error : Connect Failed \n");
        return 1;
    }
    _connected = true;

    return 0;
}

int hyperion_read()
{
    if (!sockfd)
        return -1;
    uint8_t headbuff[4];
    int n = read(sockfd, headbuff, 4);
    uint32_t messageSize =
        ((headbuff[0] << 24) & 0xFF000000) |
        ((headbuff[1] << 16) & 0x00FF0000) |
        ((headbuff[2] << 8) & 0x0000FF00) |
        ((headbuff[3]) & 0x000000FF);
    if (n < 0 || messageSize >= sizeof(recvBuff))
        return -1;
    n = read(sockfd, recvBuff, messageSize);
    if (n < 0)
        return -1;
    _parse_reply(hyperionnet_Reply_as_root(recvBuff));
    return 0;
}

int hyperion_destroy()
{
    if (!sockfd)
        return 0;
    close(sockfd);
    sockfd = 0;
    return 0;
}

int hyperion_set_image(const unsigned char *image, int width, int height)
{
    flatbuffers_builder_t B;
    flatcc_builder_init(&B);
    flatbuffers_uint8_vec_ref_t imgData = flatcc_builder_create_type_vector(&B, image, width * height * 3);
    hyperionnet_RawImage_ref_t rawImg = hyperionnet_RawImage_create(&B, imgData, width, height);
    hyperionnet_Image_ref_t imageReq = hyperionnet_Image_create(&B, hyperionnet_ImageType_as_RawImage(rawImg), -1);
    hyperionnet_Request_ref_t req = hyperionnet_Request_create_as_root(&B, hyperionnet_Command_as_Image(imageReq));
    size_t size;
    void *buf = flatcc_builder_finalize_buffer(&B, &size);
    int ret = _send_message(buf, size);
    free(buf);
    flatcc_builder_clear(&B);
    return ret;
}

int hyperion_set_register(const char *origin, int priority)
{
    if (!sockfd)
        return 0;
    flatbuffers_builder_t B;
    flatcc_builder_init(&B);
    hyperionnet_Register_ref_t registerReq = hyperionnet_Register_create(&B, flatcc_builder_create_string_str(&B, origin), priority);
    hyperionnet_Request_ref_t req = hyperionnet_Request_create_as_root(&B, hyperionnet_Command_as_Register(registerReq));

    size_t size;
    void *buf = flatcc_builder_finalize_buffer(&B, &size);
    uint8_t header[4] = {
        (uint8_t)((size >> 24) & 0xFF),
        (uint8_t)((size >> 16) & 0xFF),
        (uint8_t)((size >> 8) & 0xFF),
        (uint8_t)(size & 0xFF),
    };

    // write message
    int ret = 0;
    if (write(sockfd, header, 4) < 0)
        ret = -1;
    if (write(sockfd, buf, size) < 0)
        ret = -1;

    free(buf);
    flatcc_builder_clear(&B);
    return ret;
}

int _send_message(const void *buffer, size_t size)
{
    if (!sockfd)
        return 0;
    if (!_connected)
        return 0;

    if (!_registered)
    {
        return hyperion_set_register(_origin, _priority);
    }

    const uint8_t header[] = {
        (uint8_t)((size >> 24) & 0xFF),
        (uint8_t)((size >> 16) & 0xFF),
        (uint8_t)((size >> 8) & 0xFF),
        (uint8_t)(size & 0xFF)};

    // write message
    int ret = 0;
    if (write(sockfd, header, 4) < 0)
        ret = -1;
    if (write(sockfd, buffer, size) < 0)
        ret = -1;
    return ret;
}

bool _parse_reply(hyperionnet_Reply_table_t reply)
{
    if (!hyperionnet_Reply_error(reply))
    {
        // no error set must be a success or registered or video
        int32_t videoMode = hyperionnet_Reply_video(reply);
        int32_t registered = hyperionnet_Reply_registered(reply);
        if (videoMode != -1)
        {
            // We got a video reply.
            printf("set video mode %d\n", videoMode);
            return true;
        }

        // We got a registered reply.
        if (registered == _priority)
        {
            _registered = true;
        }

        return true;
    }
    else
    {
        flatbuffers_string_t error = hyperionnet_Reply_error(reply);
        fprintf(stderr, "Error from server: %s\n", error);
    }

    return false;
}
