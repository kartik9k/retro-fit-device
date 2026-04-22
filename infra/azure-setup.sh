#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Azure development environment setup — retro-fit device server
#
# Run this once to provision all Azure resources.
# Prerequisites:
#   1. Azure CLI installed  (https://learn.microsoft.com/en-us/cli/azure/install-azure-cli)
#   2. Logged in:           az login
#   3. Correct subscription selected (check with: az account show)
#
# Usage:
#   chmod +x infra/azure-setup.sh
#   ./infra/azure-setup.sh
#
# After it completes, the script prints:
#   - DATABASE_URL  (add to GitHub secrets + local .env)
#   - ACR credentials (add to GitHub secrets)
#   - App URL
# ---------------------------------------------------------------------------

set -euo pipefail

# ---- Configuration — edit these before running ----
RESOURCE_GROUP="retro-fit-dev"
LOCATION="uksouth"                      # change to your nearest Azure region
PG_SERVER="retro-fit-pg"
PG_USER="retrofitadmin"
PG_DB="readings"
ACR_NAME="retrofitdevacr"               # must be globally unique, lowercase, alphanumeric
CONTAINER_ENV="retro-fit-env"
CONTAINER_APP="retro-fit-server"
IMAGE_TAG="latest"
# ---------------------------------------------------

# Prompt for PG password (not stored in script)
read -rsp "Enter PostgreSQL admin password (min 8 chars, mix of upper/lower/digit/symbol): " PG_PASSWORD
echo

echo ""
echo "==> Creating resource group: $RESOURCE_GROUP in $LOCATION"
az group create --name "$RESOURCE_GROUP" --location "$LOCATION" --output none

echo "==> Creating PostgreSQL Flexible Server: $PG_SERVER"
az postgres flexible-server create \
  --resource-group "$RESOURCE_GROUP" \
  --name "$PG_SERVER" \
  --location "$LOCATION" \
  --admin-user "$PG_USER" \
  --admin-password "$PG_PASSWORD" \
  --sku-name "Standard_B1ms" \
  --tier "Burstable" \
  --storage-size 32 \
  --version 16 \
  --public-access 0.0.0.0 \
  --output none

echo "==> Creating database: $PG_DB"
az postgres flexible-server db create \
  --resource-group "$RESOURCE_GROUP" \
  --server-name "$PG_SERVER" \
  --database-name "$PG_DB" \
  --output none

echo "==> Allowing Azure services to access PostgreSQL"
az postgres flexible-server firewall-rule create \
  --resource-group "$RESOURCE_GROUP" \
  --name "$PG_SERVER" \
  --rule-name "AllowAzureServices" \
  --start-ip-address 0.0.0.0 \
  --end-ip-address 0.0.0.0 \
  --output none

echo "==> Creating Container Registry: $ACR_NAME"
az acr create \
  --resource-group "$RESOURCE_GROUP" \
  --name "$ACR_NAME" \
  --sku Basic \
  --admin-enabled true \
  --output none

echo "==> Creating Container Apps environment: $CONTAINER_ENV"
az containerapp env create \
  --name "$CONTAINER_ENV" \
  --resource-group "$RESOURCE_GROUP" \
  --location "$LOCATION" \
  --output none

# Build DATABASE_URL
DATABASE_URL="postgresql://${PG_USER}:${PG_PASSWORD}@${PG_SERVER}.postgres.database.azure.com:5432/${PG_DB}?sslmode=require"

echo "==> Creating Container App: $CONTAINER_APP (placeholder image — will be replaced by CI/CD)"
az containerapp create \
  --name "$CONTAINER_APP" \
  --resource-group "$RESOURCE_GROUP" \
  --environment "$CONTAINER_ENV" \
  --image "mcr.microsoft.com/azuredocs/containerapps-helloworld:latest" \
  --target-port 8000 \
  --ingress external \
  --min-replicas 1 \
  --max-replicas 3 \
  --env-vars "DATABASE_URL=${DATABASE_URL}" \
  --output none

# Retrieve outputs
ACR_LOGIN_SERVER=$(az acr show --name "$ACR_NAME" --query loginServer -o tsv)
ACR_USERNAME=$(az acr credential show --name "$ACR_NAME" --query username -o tsv)
ACR_PASSWORD=$(az acr credential show --name "$ACR_NAME" --query "passwords[0].value" -o tsv)
APP_URL=$(az containerapp show \
  --name "$CONTAINER_APP" \
  --resource-group "$RESOURCE_GROUP" \
  --query "properties.configuration.ingress.fqdn" -o tsv)

echo ""
echo "============================================================"
echo "  Setup complete. Add the following to GitHub Secrets:"
echo "============================================================"
echo ""
echo "  AZURE_RESOURCE_GROUP   = $RESOURCE_GROUP"
echo "  CONTAINER_APP_NAME     = $CONTAINER_APP"
echo "  CONTAINER_APP_ENV_NAME = $CONTAINER_ENV"
echo "  ACR_LOGIN_SERVER       = $ACR_LOGIN_SERVER"
echo "  ACR_USERNAME           = $ACR_USERNAME"
echo "  ACR_PASSWORD           = $ACR_PASSWORD"
echo "  DATABASE_URL           = $DATABASE_URL"
echo ""
echo "  App URL (also needed for AZURE_CREDENTIALS setup):"
echo "  https://$APP_URL"
echo ""
echo "  To create AZURE_CREDENTIALS (service principal for GitHub Actions):"
echo "  az ad sp create-for-rbac --name retro-fit-deploy \\"
echo "    --role contributor \\"
echo "    --scopes /subscriptions/\$(az account show --query id -o tsv)/resourceGroups/$RESOURCE_GROUP \\"
echo "    --sdk-auth"
echo "  Copy the JSON output as the AZURE_CREDENTIALS secret."
echo ""
echo "  Seed script (Mode 2 — dummy data to Azure):"
echo "  cd server && python seed.py --url https://$APP_URL/api/data"
echo "============================================================"
