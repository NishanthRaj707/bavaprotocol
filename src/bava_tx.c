#include <bava.h>

extern uint16_t bava_calculate_crc(uint8_t cmd, uint8_t id, uint8_t len, uint8_t *payload);

static void bava_add_escape_byte(uint8_t* buffer, uint16_t* idx, uint8_t byte)
{
    if (byte == 0xAA || byte == 0x7D)
    {
        buffer[(*idx)++] = 0x7D;
        buffer[(*idx)++] = byte ^ 0x20;
    }
    else
    {
        buffer[(*idx)++] = byte;
    }
}

static void bava_internal_send_packet(bava_ctx_t *ctx, uint8_t cmd, uint8_t id, const uint8_t *payload, uint8_t len)
{
    uint16_t tx_idx = 0;

    ctx->tx_buffer[tx_idx++] = 0xAA;
    ctx->tx_buffer[tx_idx++] = 0x55;

    bava_add_escape_byte(ctx->tx_buffer, &tx_idx, cmd);
    bava_add_escape_byte(ctx->tx_buffer, &tx_idx, id);
    bava_add_escape_byte(ctx->tx_buffer, &tx_idx, len);

    for (uint8_t i = 0; i < len; i++)
    {
        bava_add_escape_byte(ctx->tx_buffer, &tx_idx, payload[i]);
    }
    
    uint16_t crc = bava_calculate_crc(cmd, id, len, (uint8_t *)payload);

    uint8_t crc_low = (uint8_t)(crc & 0xFF);
    uint8_t crc_high = (uint8_t)((crc >> 8) & 0xFF);
    
    bava_add_escape_byte(ctx->tx_buffer, &tx_idx, crc_low);
    bava_add_escape_byte(ctx->tx_buffer, &tx_idx, crc_high);

    if (ctx->tx_callback != NULL)
    {
        ctx->tx_callback(ctx->tx_buffer, tx_idx);
    }
    ctx->is_waiting_ack = true;
    ctx->pending_ack_id = id;
    ctx->pending_ack_cmd = cmd;
}

void bava_send_write(bava_ctx_t* ctx, uint8_t id)
{
    for (uint8_t i = 0; i < ctx->var_count; i++)
    {
        if (ctx->variables[i].id == id)
        {
            const uint8_t* payload = (const uint8_t *)ctx->variables[i].var_ptr;
            uint8_t len = ctx->variables[i].size;

            bava_internal_send_packet(ctx, BAVA_WRITE, id, payload, len);
            return;
        }
    }
}

void bava_send_read(bava_ctx_t* ctx, uint8_t id)
{
    bava_internal_send_packet(ctx, BAVA_READ, id, NULL, 0);
}

void bava_send_raw_write(bava_ctx_t* ctx, uint8_t id, const uint8_t* pointer, uint8_t len)
{
    bava_internal_send_packet(ctx, BAVA_WRITE, id, pointer, len);
}

void bava_cmd_write_ack(bava_ctx_t* ctx, uint8_t id)
{
    bava_internal_send_packet(ctx, BAVA_WRITE_ACK, id, NULL, 0);
}

void bava_cmd_read_request(bava_ctx_t* ctx, uint8_t id)
{
    for (uint8_t i = 0; i < ctx->var_count; i++)
    {
        if (ctx->variables[i].id == id)
        {
            const uint8_t* payload = (const uint8_t *)ctx->variables[i].var_ptr;
            uint8_t len = ctx->variables[i].size;
            bava_internal_send_packet(ctx, BAVA_READ_RESP, id, payload, len);
            return;
        }
    }
}
